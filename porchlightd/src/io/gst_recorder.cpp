#include "io/gst_recorder.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <system_error>
#include <utility>

#include "child_process.h"
#include "logging.h"

namespace porch {
namespace {

// How long the muxer gets to write its moov atom after SIGINT before we stop
// being polite. The systemd unit's TimeoutStopSec is set above this.
constexpr std::chrono::seconds kEosGrace{5};

// An MP4 with a header and nothing else is still bigger than this, so anything
// smaller did not survive its muxer.
constexpr std::uintmax_t kMinPlausibleBytes = 1024;

// The full sensor array on an imx708. libcamera otherwise picks the binned
// 1536x864 mode for any small request, and that mode reads only the centre
// 3072x1728 of the 4608x2592 array - on the Camera Module 3 *Wide* that throws
// away about a third of the frame width, which is the field of view the wide
// lens exists for. 2304x1296 covers the whole array and still runs at 56 fps,
// so it carries 30 with room to spare; the ISP scales it to whatever size the
// config asks for.
constexpr const char* kFullFrameSensorMode =
    "sensor-config=\"sensor/config,width=2304,height=1296,depth=10\"";

void add_video(std::vector<std::string>& args, const RecorderConfig& config) {
  const bool camera = config.video_source == "libcamera";
  if (camera) {
    args.push_back("libcamerasrc");
    // A Camera Module 3 starts in *manual* focus at lens-position 0, and 0
    // dioptres is infinity - so an unconfigured camera films the horizon while
    // whoever rang the bell stands a metre away. Continuous is the safe choice
    // while the mounting distance is unknown; a fixed lens-position is better
    // once the camera stops moving, because AF hunts on a static scene.
    args.push_back("af-mode=continuous");
    args.push_back(kFullFrameSensorMode);
  } else {
    args.push_back("videotestsrc");
    args.push_back("is-live=true");
    args.push_back("pattern=ball");
  }
  args.push_back("!");
  // format=I420 only on the camera branch: left to itself libcamerasrc
  // negotiates NV21, and the videoconvert below then de-interleaves chroma on
  // every frame instead of passing it straight through. videotestsrc already
  // produces I420, so pinning it there would say nothing.
  args.push_back(std::format("video/x-raw,{}width={},height={},framerate={}/1",
                             camera ? "format=I420," : "", config.width, config.height,
                             config.fps));
  args.push_back("!");
  // Gives the source a thread of its own, so encoding never stalls capture.
  args.push_back("queue");
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
  // And another before the muxer, so a muxer waiting on the other branch does
  // not reach back and block this one.
  args.push_back("queue");
  args.push_back("!");
  args.push_back("mux.");
}

void add_audio(std::vector<std::string>& args, const RecorderConfig& config) {
  if (config.audio_source == "alsa") {
    args.push_back("alsasrc");
    // The device name contains an '=', so the parser needs it quoted.
    args.push_back(std::format("device=\"{}\"", config.audio_device));
    // alsasrc offers the sound card as the pipeline clock and libcamerasrc does
    // not offer one at all, so with both present GStreamer picks the card's.
    // libcamerasrc goes on timestamping from the system monotonic clock
    // whatever the pipeline chose, so every video buffer ends up with a running
    // time worked out by subtracting a base time in one clock's units from a
    // timestamp in another's. In webrtc-video.py that stalls the video branch
    // within a second while audio carries on perfectly and hides the cause.
    //
    // That fix is pipeline.use_clock(), and there is no such call from a
    // gst-launch command line - so the sound card declines the job instead and
    // the pipeline falls back to the system clock. Only when the camera is the
    // source: videotestsrc paces itself off whichever clock was chosen, so the
    // tested videotestsrc + alsasrc path keeps the behaviour it was verified
    // with, including its 200 ms buffer.
    if (config.video_source == "libcamera") {
      args.push_back("provide-clock=false");
    }
    // Deliberately not the 40 ms that webrtc-video.py uses. A call trades
    // buffer for latency; a recording has no latency requirement at all, and
    // 40 ms of slack cost this Pi a quarter of its samples.
    args.push_back("buffer-time=200000");
    args.push_back("latency-time=20000");
  } else {
    args.push_back("audiotestsrc");
    args.push_back("is-live=true");
  }
  args.push_back("!");
  // The important one. Without it the capture thread also does the resampling,
  // the AAC encode and the push into the muxer, and the sound card overruns
  // while it is busy elsewhere.
  args.push_back("queue");
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
  args.push_back("queue");
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

  // Said once at startup rather than per clip. Neither is fatal and neither is
  // this code's business to override - a wrong aspect still records, and a
  // stalled encoder is a thing to see reported rather than guessed at.
  if (config_.video_source == "libcamera") {
    if (config_.width * 9 != config_.height * 16) {
      log(Level::Warn, "rec",
          "{}x{} is not 16:9 but the imx708 is, so the ISP will crop the sides off - "
          "on the wide lens that is the field of view it exists for. 640x360 or 1280x720.",
          config_.width, config_.height);
    }
    if (config_.encoder == "v4l2") {
      log(Level::Warn, "rec",
          "encoder=v4l2 stalls when fed by libcamerasrc - even a bare "
          "libcamerasrc ! videoconvert ! v4l2h264enc ! fakesink produces nothing, where "
          "x264enc runs at 30 fps. x264 is the tested path with the camera.");
    }
  }
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

  const pid_t pid = spawn_child(build_pipeline(config_, output_));
  if (pid < 0) {
    log(Level::Error, "rec", "cannot run gst-launch-1.0: {}", std::strerror(errno));
    report(false);
    return;
  }

  pid_ = pid;
  pidfd_ = watch_child(pid_);
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
