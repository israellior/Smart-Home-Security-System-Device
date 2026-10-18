#include "io/gst_recorder.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

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
    "sensor-config=sensor/config,width=2304,height=1296,depth=10";

// A Camera Module 3 starts in *manual* focus at lens-position 0, and 0 dioptres
// is infinity - so an unconfigured camera films the horizon while whoever rang
// the bell stands a metre away. Continuous is the safe choice while the
// mounting distance is unknown; a fixed lens-position is better once the camera
// stops moving, because AF hunts on a static scene.
constexpr const char* kContinuousFocus = "af-mode=continuous";

// The element record-clip.py looks up to set the two above.
constexpr const char* kCameraName = "camera";

void add_video(std::vector<std::string>& args, const RecorderConfig& config, bool detect) {
  const bool camera = config.video_source == "libcamera";
  if (camera) {
    args.push_back("libcamerasrc");
    // Named, and nothing else: sensor-config exists only in Raspberry Pi's
    // libcamera, and a property the element lacks fails the whole parse. Both
    // go to record-clip.py as --camera-property instead, which sets each only
    // if list_properties() has it - the Buildroot image's upstream libcamera
    // recorded nothing at all until they moved.
    args.push_back(std::format("name={}", kCameraName));
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
  if (detect) {
    // The detector cannot open the camera itself - only one process can hold
    // it - so it gets a copy of these frames instead. Its branch is added in
    // add_detector, after the muxer's branches are complete.
    args.push_back("tee");
    args.push_back("name=t");
    args.push_back("!");
  }
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
    // No provide-clock=false. It was here to stop the card becoming the
    // pipeline clock, on the theory that it was the gst-launch equivalent of
    // use_clock(), and on the Pi it is not: camera + card recorded ten video
    // frames spread over 204 hours with it, and with slave-method=none, and
    // with a 40 ms buffer. Only the pinned clock recorded a clip. That is
    // --system-clock below, in record-clip.py.
    //
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

// The second branch of the tee. Everything about it serves one rule: the
// detector must never be able to slow the clip down. A Pi that is hot, or a
// model that is slower than expected, should cost detected frames and never a
// recorded one - so the queue throws the oldest frame away rather than
// blocking the tee, and the appsink keeps only the newest.
void add_detector(std::vector<std::string>& args, const DetectConfig& detect) {
  args.push_back("t.");
  args.push_back("!");
  args.push_back("queue");
  args.push_back("leaky=downstream");
  args.push_back("max-size-buffers=1");
  args.push_back("max-size-bytes=0");
  args.push_back("max-size-time=0");
  args.push_back("!");
  // Before the scaling, so the frames that will be dropped anyway are never
  // converted.
  args.push_back("videorate");
  args.push_back("drop-only=true");
  args.push_back(std::format("max-rate={}", detect.fps));
  args.push_back("!");
  // Scaled and converted here, in C, rather than in Python. Squashed rather
  // than letterboxed: on porch-2, 2026-10-18, letterboxing and two square
  // tiles scored a person within 0.02 of it, and they cost more.
  args.push_back("videoscale");
  args.push_back("!");
  args.push_back("videoconvert");
  args.push_back("!");
  args.push_back(std::format("video/x-raw,format=RGB,width={0},height={0}", detect.input_size));
  args.push_back("!");
  args.push_back("appsink");
  args.push_back("name=detect");
  args.push_back("drop=true");
  args.push_back("max-buffers=1");
  // As fast as frames come. Syncing would hold each one until its timestamp,
  // which buys nothing for a frame that is only going to be looked at.
  args.push_back("sync=false");
}

// The camera's second stream, at the sensor mode's own 2304x1296, for faces:
// a face a metre from the door is about 155 pixels wide here and about 45 in
// the recording, which is too few to tell anyone apart. Nothing reads it
// until the detector has found a person, so the appsink keeps one frame and
// drops the rest, and none of them is ever mapped or copied.
//
// Its own queue first, and that is not tidiness: libcamerasrc pushes both
// streams from one thread, and a branch without a queue holds the pipeline in
// PAUSED - no frame on either pad, no error either.
void add_details(std::vector<std::string>& args) {
  args.push_back(std::format("{}.src_0", kCameraName));
  args.push_back("!");
  args.push_back("video/x-raw,format=I420,width=2304,height=1296");
  args.push_back("!");
  args.push_back("queue");
  args.push_back("leaky=downstream");
  args.push_back("max-size-buffers=1");
  args.push_back("max-size-bytes=0");
  args.push_back("max-size-time=0");
  args.push_back("!");
  args.push_back("appsink");
  args.push_back("name=detail");
  args.push_back("drop=true");
  args.push_back("max-buffers=1");
  args.push_back("sync=false");
}

}  // namespace

// Rebuilt rather than passed through: the line comes from a script, and what
// goes on to the server is only ever a few known fields. The server keeps
// `meta` as given up to 2 KB and drops the lot beyond that, so four faces is
// a ceiling well inside it, and a doorbell has never had five at once.
std::string faces_of(const nlohmann::json& message) {
  const auto it = message.find("faces");
  if (it == message.end() || !it->is_array()) {
    return {};
  }
  nlohmann::json clean = nlohmann::json::array();
  for (const auto& face : *it) {
    if (!face.is_object() || clean.size() >= 4) {
      continue;
    }
    const auto person = face.find("person");
    const auto score = face.find("score");
    nlohmann::json entry = nlohmann::json::object();
    entry["person"] = (person != face.end() && person->is_string() && person->size() <= 64)
                          ? *person
                          : nlohmann::json(nullptr);
    entry["score"] = (score != face.end() && score->is_number()) ? score->get<double>() : 0.0;
    clean.push_back(entry);
  }
  return clean.dump();
}

std::vector<std::string> build_pipeline(const RecorderConfig& config,
                                        const std::filesystem::path& output,
                                        std::chrono::seconds detect_for) {
  const bool detect = detect_for > std::chrono::seconds::zero();
  // Faces need a person first, and the camera's second stream to find them in.
  const bool faces = detect && config.detect.faces && config.video_source == "libcamera";

  // record-clip.py is gst-launch-1.0 -e with one addition: it turns SIGINT into
  // EOS, which flushes the muxer and leaves a file that will play, and it can
  // pin the pipeline clock, which gst-launch cannot.
  //
  // Run by its shebang normally, and by the venv's interpreter when it
  // detects, because the inference runtime is a pip package.
  std::vector<std::string> args;
  if (detect) {
    args.push_back(config.detect.python.string());
  }
  args.push_back(config.script.string());

  // libcamerasrc stamps frames against the system monotonic clock whatever the
  // pipeline chose, and alsasrc offers the card's clock, so with both present
  // the video track is garbage - ten frames over 204 hours, on the Pi. Only
  // with the camera: videotestsrc paces itself off whichever clock was chosen,
  // so the videotestsrc + alsasrc path keeps the clock it was verified with.
  if (config.video_source == "libcamera") {
    args.push_back("--system-clock");
    for (const char* property : {kContinuousFocus, kFullFrameSensorMode}) {
      args.push_back("--camera-property");
      args.push_back(property);
    }
  }
  if (detect) {
    const DetectConfig& d = config.detect;
    args.insert(args.end(), {
        "--detect-model", d.model.string(),
        "--detect-size", std::to_string(d.input_size),
        "--detect-window", std::to_string(detect_for.count()),
        "--detect-min-score", std::to_string(d.min_score_percent),
        "--detect-min-luma", std::to_string(d.min_luma),
        "--detect-min-frames", std::to_string(d.min_frames),
    });
  }
  if (faces) {
    const DetectConfig& d = config.detect;
    args.insert(args.end(), {
        "--faces-store", d.faces_store.string(),
        "--face-match", std::to_string(d.face_match_percent),
        "--face-margin", std::to_string(d.face_margin_percent),
    });
  }
  args.push_back("--");

  args.push_back("mp4mux");
  args.push_back("name=mux");
  // Puts the moov atom at the front, so the clip plays before it has finished
  // downloading.
  args.push_back("faststart=true");
  args.push_back("!");
  args.push_back("filesink");
  args.push_back(std::format("location=\"{}\"", output.string()));

  add_video(args, config, detect);
  add_audio(args, config);
  if (detect) {
    add_detector(args, config.detect);
  }
  if (faces) {
    add_details(args);
  }
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

  // Not fatal: without a model every motion comes back unknown, and unknown
  // alerts, so the doorbell behaves exactly as it did before detection. But
  // that looks like a detector that never finds anybody, so say why once.
  if (config_.detect.enabled) {
    std::error_code ec;
    if (!std::filesystem::exists(config_.detect.model, ec)) {
      log(Level::Warn, "rec",
          "detection is on but there is no model at {} - every motion will alert unjudged",
          config_.detect.model.string());
    }
    if (!std::filesystem::exists(config_.detect.python, ec)) {
      log(Level::Warn, "rec",
          "detection is on but there is no interpreter at {} - motion recordings will fail "
          "to start, and alert unjudged",
          config_.detect.python.string());
    }
    // Also not fatal, and the ordinary state of a doorbell nobody has saved a
    // face on: a person alerts as a person, with no name.
    if (config_.detect.faces && !std::filesystem::exists(config_.detect.faces_store, ec)) {
      log(Level::Info, "rec", "faces are on but none are saved yet ({})",
          config_.detect.faces_store.string());
    }
    if (config_.detect.faces && config_.video_source != "libcamera") {
      log(Level::Warn, "rec", "faces are on but the video is not the camera - not looked for");
    }
  }

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

void GstRecorder::start(const EventId& event_id, std::chrono::seconds seconds,
                        std::chrono::seconds detect_for) {
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
  verdict_sent_ = false;
  detector_input_.clear();

  const std::vector<std::string> args = build_pipeline(config_, output_, detect_for);
  pid_t pid = -1;
  if (detect_for > std::chrono::seconds::zero()) {
    PipedChild child = spawn_child_with_pipes(args);
    pid = child.pid;
    if (pid >= 0) {
      // Nothing is ever said to the recorder on stdin; closing our end says so.
      child.to_child.reset();
      detector_ = std::move(child.from_child);
      // Non-blocking, because it is drained once more after the child exits,
      // and anything the child spawned may still hold the write end open.
      ::fcntl(detector_.get(), F_SETFL, O_NONBLOCK);
      reactor_.watch(detector_.get(), [this] { read_detector(); });
    }
  } else {
    pid = spawn_child(args);
  }
  if (pid < 0) {
    log(Level::Error, "rec", "cannot run {}: {}", args.front(), std::strerror(errno));
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
    release_child();
    report(false);
    return;
  }

  // The child exiting is just another readable descriptor.
  reactor_.watch(pidfd_.get(), [this] { on_child_exit(); });
  timer_.arm_in(seconds);
  phase_ = Phase::Recording;

  if (detect_for > std::chrono::seconds::zero()) {
    log(Level::Info, "rec", "recording id={} for {}s to {}, looking for a person for {}s",
        event_id, seconds.count(), output_.string(), detect_for.count());
  } else {
    log(Level::Info, "rec", "recording id={} for {}s to {}", event_id, seconds.count(),
        output_.string());
  }
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
    log(Level::Warn, "rec", "no EOS after {}s; killing the recorder and giving up on {}",
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
  // The pidfd and the pipe are two descriptors and epoll may report either
  // first. A verdict still sitting in the pipe must reach the core before the
  // RecordingFinished does, or the core reads the silence as Unknown.
  read_detector();
  release_child();

  const bool clean = !killed_ && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!clean) {
    log(Level::Warn, "rec", "the recorder ended badly for {} (status {})", event_id_, status);
  }
  report(clean);
}

void GstRecorder::release_child() {
  timer_.disarm();
  if (pidfd_.valid()) {
    reactor_.unwatch(pidfd_.get());
    pidfd_.reset();
  }
  if (detector_.valid()) {
    reactor_.unwatch(detector_.get());
    detector_.reset();
  }
  detector_input_.clear();
  pid_ = -1;
}

void GstRecorder::read_detector() {
  if (!detector_.valid()) {
    return;
  }
  char chunk[512];
  for (;;) {
    const ssize_t got = ::read(detector_.get(), chunk, sizeof(chunk));
    if (got > 0) {
      detector_input_.append(chunk, static_cast<std::size_t>(got));
      continue;
    }
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got == 0 || errno != EAGAIN) {
      // The child closed its stdout - it is exiting. The pidfd says so too,
      // and that is where the cleanup happens; this only stops listening.
      reactor_.unwatch(detector_.get());
      detector_.reset();
    }
    break;
  }

  std::size_t newline = detector_input_.find('\n');
  while (newline != std::string::npos) {
    const std::string line = detector_input_.substr(0, newline);
    detector_input_.erase(0, newline + 1);
    handle_detector_line(line);
    newline = detector_input_.find('\n');
  }
}

void GstRecorder::handle_detector_line(const std::string& line) {
  const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
  if (!message.is_object() || !message.contains("verdict") || !message["verdict"].is_string()) {
    log(Level::Warn, "rec", "detector said something unexpected: {}", line);
    return;
  }

  const std::string said = message["verdict"].get<std::string>();
  Verdict verdict = Verdict::Unknown;
  if (said == "person") {
    verdict = Verdict::Person;
  } else if (said == "no-person") {
    verdict = Verdict::NoPerson;
  } else if (said != "unknown") {
    // A word this side does not know is not permission to stay silent.
    log(Level::Warn, "rec", "detector verdict '{}' is not one we know; taking it as unknown",
        said);
  }

  // One per recording, which is the promise the core relies on. A second one
  // means the script is broken; the first answer stands.
  if (verdict_sent_) {
    log(Level::Warn, "rec", "a second verdict for {} ignored: {}", event_id_, line);
    return;
  }
  verdict_sent_ = true;
  const std::string faces = faces_of(message);
  log(Level::Info, "rec", "detector: {} for {} ({}){}", to_string(verdict), event_id_,
      message.value("why", std::string{"no reason given"}),
      faces.empty() ? std::string{} : std::format("; faces {}", faces));
  sink_(DetectionResult{event_id_, verdict, faces});
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
