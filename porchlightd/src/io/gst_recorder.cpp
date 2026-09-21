#include "io/gst_recorder.h"

#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <format>
#include <system_error>
#include <utility>

#include "logging.h"

extern char** environ;

namespace porch {
namespace {

// How long the muxer gets to write its moov atom after SIGINT before we stop
// being polite. The systemd unit's TimeoutStopSec is set above this.
constexpr std::chrono::seconds kEosGrace{5};

// An MP4 with a header and nothing else is still bigger than this, so anything
// smaller did not survive its muxer.
constexpr std::uintmax_t kMinPlausibleBytes = 1024;

int open_pidfd(pid_t pid) {
  return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0u));
}

void add_video(std::vector<std::string>& args, const RecorderConfig& config) {
  if (config.video_source == "libcamera") {
    args.push_back("libcamerasrc");
  } else {
    args.push_back("videotestsrc");
    args.push_back("is-live=true");
    args.push_back("pattern=ball");
  }
  args.push_back("!");
  args.push_back(std::format("video/x-raw,width={},height={},framerate={}/1", config.width,
                             config.height, config.fps));
  args.push_back("!");
  args.push_back("videoconvert");
  args.push_back("!");

  if (config.encoder == "v4l2") {
    args.push_back("v4l2h264enc");
    // Quoted because the value itself contains commas and '=', which the
    // pipeline parser would otherwise read as more properties.
    args.push_back(std::format(
        "extra-controls=\"controls,repeat_sequence_header=1,h264_i_frame_period={},"
        "video_bitrate={}\"",
        config.fps, config.video_bitrate_kbps * 1000));
    args.push_back("!");
    args.push_back("video/x-h264,level=(string)4");
  } else {
    args.push_back("x264enc");
    args.push_back("tune=zerolatency");
    args.push_back("speed-preset=ultrafast");
    args.push_back(std::format("bitrate={}", config.video_bitrate_kbps));
    args.push_back(std::format("key-int-max={}", config.fps));
    args.push_back("!");
    args.push_back("video/x-h264,profile=constrained-baseline");
  }

  args.push_back("!");
  args.push_back("h264parse");
  args.push_back("!");
  args.push_back("mux.");
}

void add_audio(std::vector<std::string>& args, const RecorderConfig& config) {
  if (config.audio_source == "alsa") {
    args.push_back("alsasrc");
    // The device name contains an '=', so the parser needs it quoted.
    args.push_back(std::format("device=\"{}\"", config.audio_device));
    // alsasrc defaults to a 200 ms buffer. CLAUDE.md records this Pi
    // complaining it "can't record audio fast enough" - raise these if it does.
    args.push_back("buffer-time=40000");
    args.push_back("latency-time=10000");
  } else {
    args.push_back("audiotestsrc");
    args.push_back("is-live=true");
  }
  args.push_back("!");
  args.push_back("audioconvert");
  args.push_back("!");
  args.push_back("audioresample");
  args.push_back("!");
  // The HAT is a stereo codec with two microphones; a doorbell clip is mono.
  args.push_back("audio/x-raw,rate=48000,channels=1");
  args.push_back("!");
  args.push_back(config.aac_element);
  args.push_back("!");
  args.push_back("aacparse");
  args.push_back("!");
  args.push_back("mux.");
}

}  // namespace

std::vector<std::string> build_pipeline(const RecorderConfig& config,
                                        const std::filesystem::path& output) {
  // -e is the whole reason this works: it turns SIGINT into EOS, and EOS is
  // what flushes the muxer and leaves a file that will play.
  std::vector<std::string> args{"gst-launch-1.0", "-e"};

  args.push_back("mp4mux");
  args.push_back("name=mux");
  // Puts the moov atom at the front, so the clip plays before it has finished
  // downloading.
  args.push_back("faststart=true");
  args.push_back("!");
  args.push_back("filesink");
  args.push_back(std::format("location=\"{}\"", output.string()));

  add_video(args, config);
  add_audio(args, config);
  return args;
}

GstRecorder::GstRecorder(Reactor& reactor, EventSink sink, RecorderConfig config,
                         std::filesystem::path spool)
    : reactor_(reactor),
      sink_(std::move(sink)),
      config_(std::move(config)),
      spool_(std::move(spool)) {
  reactor_.watch(timer_.fd(), [this] {
    timer_.drain();
    on_timer();
  });
}

GstRecorder::~GstRecorder() {
  if (pid_ > 0) {
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
  }
  release_child();
  reactor_.unwatch(timer_.fd());
}

void GstRecorder::start(const EventId& event_id, std::chrono::seconds seconds) {
  if (phase_ != Phase::Idle) {
    // The core never does this. If it ever did, the answer must still be
    // exactly one RecordingFinished per start, or it would wait forever.
    log(Level::Error, "rec", "start for {} while {} is still running", event_id, event_id_);
    sink_(RecordingFinished{event_id, "", false, std::chrono::milliseconds{0}, 0});
    return;
  }

  std::error_code ec;
  std::filesystem::create_directories(spool_, ec);
  if (ec) {
    log(Level::Error, "rec", "cannot create spool {}: {}", spool_.string(), ec.message());
    sink_(RecordingFinished{event_id, "", false, std::chrono::milliseconds{0}, 0});
    return;
  }

  event_id_ = event_id;
  output_ = spool_ / std::format("{}.mp4", event_id);
  started_at_ = Clock::now();
  killed_ = false;

  const std::vector<std::string> args = build_pipeline(config_, output_);
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  // Spawned directly, never through a shell: a shell would receive the SIGINT
  // meant for gst-launch and the clip would never get its EOS.
  pid_t pid = -1;
  const int failure = ::posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
  if (failure != 0) {
    log(Level::Error, "rec", "cannot run gst-launch-1.0: {}", std::strerror(failure));
    report(false);
    return;
  }

  pid_ = pid;
  pidfd_.reset(open_pidfd(pid_));
  if (!pidfd_.valid()) {
    log(Level::Error, "rec", "pidfd_open failed: {}", std::strerror(errno));
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    report(false);
    return;
  }

  // The child exiting is just another readable descriptor.
  reactor_.watch(pidfd_.get(), [this] { on_child_exit(); });
  timer_.arm_in(seconds);
  phase_ = Phase::Recording;

  log(Level::Info, "rec", "recording id={} for {}s to {}", event_id, seconds.count(),
      output_.string());
}

void GstRecorder::stop() { begin_stop(); }

void GstRecorder::begin_stop() {
  if (phase_ != Phase::Recording) {
    return;  // already flushing, or nothing to stop
  }
  phase_ = Phase::Stopping;
  if (pid_ > 0) {
    ::kill(pid_, SIGINT);
  }
  timer_.arm_in(kEosGrace);
}

void GstRecorder::on_timer() {
  if (phase_ == Phase::Recording) {
    begin_stop();  // the clip reached its length
    return;
  }
  if (phase_ == Phase::Stopping) {
    log(Level::Warn, "rec", "no EOS after {}s; killing gst-launch and giving up on {}",
        kEosGrace.count(), event_id_);
    killed_ = true;
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
    }
  }
}

void GstRecorder::on_child_exit() {
  int status = 0;
  ::waitpid(pid_, &status, 0);  // already exited, so this does not block
  release_child();

  const bool clean = !killed_ && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!clean) {
    log(Level::Warn, "rec", "gst-launch ended badly for {} (status {})", event_id_, status);
  }
  report(clean);
}

void GstRecorder::release_child() {
  timer_.disarm();
  if (pidfd_.valid()) {
    reactor_.unwatch(pidfd_.get());
    pidfd_.reset();
  }
  pid_ = -1;
}

void GstRecorder::report(bool clean_exit) {
  const auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at_);

  std::error_code ec;
  const bool exists = std::filesystem::exists(output_, ec) && !ec;
  std::uintmax_t bytes = 0;
  if (exists) {
    bytes = std::filesystem::file_size(output_, ec);
    if (ec) {
      bytes = 0;
    }
  }

  RecordingFinished finished;
  finished.event_id = event_id_;
  // Nothing on disk means nothing for anyone to delete either.
  finished.path = exists ? output_.string() : std::string{};
  // A killed muxer leaves a file with no moov atom, so exiting cleanly and
  // having produced something are both required before calling it playable.
  finished.ok = clean_exit && bytes >= kMinPlausibleBytes;
  finished.duration = duration;
  finished.bytes = bytes;

  log(Level::Info, "rec", "finished id={} ok={} duration={}ms bytes={}", finished.event_id,
      finished.ok, duration.count(), bytes);

  phase_ = Phase::Idle;
  event_id_.clear();
  sink_(std::move(finished));
}

}  // namespace porch
