#include "io/script_media.h"

#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <utility>

#include "child_process.h"
#include "logging.h"

namespace porch {
namespace {

// How long the call gets to leave the room and let go of the camera and the
// sound card after SIGTERM. It has an ALSA device and a camera open, and both
// go to one process at a time - a recording started before it has actually
// exited fails to open either.
constexpr std::chrono::seconds kStopGrace{5};

// A viewer asked for the call inside this window before the child exited, so
// the request almost certainly arrived after the script had already decided it
// was finished. Starting another is right; reporting the call over is not.
constexpr std::chrono::seconds kRaceWindow{5};

// webrtc-video.py's EXIT_CONFIG: a missing GStreamer element, no camera, or a
// credential LiveKit's token endpoint refused outright. All of them fail the
// same way on the next try, so the one thing not to do is try again. Without
// this, a viewer pressing Watch on a device whose camera is missing would start
// a process per press, each of which dies immediately.
constexpr int kConfigExit = 1;

}  // namespace

std::vector<std::string> build_media_command(const ServerConfig& server, const MediaConfig& media,
                                             const std::string& device_id) {
  std::vector<std::string> args{
      // The interpreter, not the script. The LiveKit SDK is a pip package and
      // python3-gi is an apt one, so this has to be a venv built with
      // --system-site-packages; running the script directly would find the
      // system python and fail on the import.
      media.python.string(),
      media.script.string(),
      "--url", server.base_url,
      "--device-id", device_id,
      // A path, never the credential itself. Arguments are world-readable in
      // /proc, so a secret on a command line is a secret published to every
      // account on the machine.
      "--credential-file", server.credential_path.string(),
      "--video", media.video_source,
      "--size", std::format("{}x{}", media.width, media.height),
      "--fps", std::to_string(media.fps),
      "--codec", media.codec,
      "--bitrate-kbps", std::to_string(media.video_bitrate_kbps),
      "--audio", media.audio_source,
      "--aec", media.echo_cancel ? "on" : "off",
      "--idle-timeout", std::to_string(media.idle_timeout.count()),
      "--linger", std::to_string(media.linger.count()),
      // How long a call may spend rejoining before it gives up. The script
      // mints a fresh token per attempt, so this is a budget for bad network
      // and nothing else - there is no token here to expire.
      "--reconnect-timeout", std::to_string(media.reconnect_timeout.count()),
  };

  // Only where it means something: --focus on a fixed-lens camera and
  // --mic-device with no microphone are both arguments that describe nothing.
  if (media.video_source == "camera") {
    args.push_back("--focus");
    args.push_back(media.focus);
  }
  if (media.audio_source == "alsa") {
    args.push_back("--mic-device");
    args.push_back(media.mic_device);
  }
  if (media.talkback) {
    args.push_back("--speaker-device");
    args.push_back(media.speaker_device);
  } else {
    args.push_back("--no-talkback");
  }
  return args;
}

ScriptMedia::ScriptMedia(Reactor& reactor, EventSink sink, ServerConfig server, MediaConfig media,
                         std::string device_id)
    : reactor_(reactor),
      sink_(std::move(sink)),
      server_(std::move(server)),
      media_(std::move(media)),
      device_id_(std::move(device_id)) {
  reactor_.watch(timer_.fd(), [this] {
    timer_.drain();
    on_timer();
  });

  // Said once at startup rather than per call, and for the same reason the
  // recorder says it: a wrong aspect ratio still works, and nobody would ever
  // guess why the picture looks narrower than the recordings.
  if (media_.video_source == "camera" && media_.width * 9 != media_.height * 16) {
    log(Level::Warn, "media",
        "{}x{} is not 16:9 but the imx708 is, so the ISP will crop the sides off - "
        "on the wide lens that is the field of view it exists for. 640x360 or 1280x720.",
        media_.width, media_.height);
  }
  if (media_.talkback && media_.audio_source == "alsa" && !media_.echo_cancel &&
      media_.speaker_device == media_.mic_device) {
    log(Level::Warn, "media",
        "the microphone and the speaker are one card and echo cancellation is off, "
        "so the caller will hear themselves and it can howl. media.echo_cancel true.");
  }
}

ScriptMedia::~ScriptMedia() {
  // Polite even here, unlike the recorder, and for a reason the recorder does
  // not have: a call killed outright stays in the LiveKit room until the server
  // times it out, so whoever was watching keeps a frozen picture for several
  // seconds after the daemon has gone. SIGTERM lets it leave properly.
  //
  // This blocks, which is only acceptable because the reactor has already
  // stopped and the process is on its way out. The bound is what keeps it
  // inside the unit's TimeoutStopSec.
  if (pid_ > 0) {
    ::kill(pid_, SIGTERM);
    for (int waited = 0; waited < 300; ++waited) {  // 3s, in 10ms steps
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        break;
      }
      const timespec step{0, 10 * 1000 * 1000};
      ::nanosleep(&step, nullptr);
    }
    if (pid_ > 0) {
      log(Level::Warn, "media", "the call would not leave; killing it");
      ::kill(pid_, SIGKILL);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
  }
  release_child();
  reactor_.unwatch(timer_.fd());
}

void ScriptMedia::start_call(const PeerId& peer) {
  peers_.insert(peer);
  requested_at_ = Clock::now();

  if (stopping_) {
    // On its way out with a viewer already asking for it back. The exit is
    // what starts the next one; anything sooner fights over the camera.
    log(Level::Info, "media", "peer={} asked while the last call was still stopping", peer);
    return;
  }
  if (pid_ > 0) {
    // Nothing to do, and that is the point of an SFU: the Pi publishes one
    // stream whatever the size of the audience.
    log(Level::Info, "media", "peer={} joins the call already running ({} viewer(s))", peer,
        peers_.size());
    return;
  }
  if (!spawn()) {
    // Nobody is going to report this call over if it never began, and a viewer
    // the core still believes in keeps the LED lit and holds every clip upload
    // back for as long as the daemon runs.
    end_for_everyone("could not be started");
  }
}

bool ScriptMedia::spawn() {
  const std::vector<std::string> args = build_media_command(server_, media_, device_id_);

  const pid_t pid = spawn_child(args);
  if (pid < 0) {
    log(Level::Error, "media", "cannot run {}: {}", media_.script.string(),
        std::strerror(errno));
    return false;
  }

  pid_ = pid;
  pidfd_ = watch_child(pid_);
  if (!pidfd_.valid()) {
    log(Level::Error, "media", "pidfd_open failed: {}", std::strerror(errno));
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    return false;
  }

  reactor_.watch(pidfd_.get(), [this] { on_child_exit(); });
  log(Level::Info, "media", "call starting for {} viewer(s), publishing to LiveKit",
      peers_.size());
  return true;
}

void ScriptMedia::stop_call() {
  if (pid_ <= 0 || stopping_) {
    return;
  }
  stopping_ = true;
  // Polite first: the script leaves the room, unpublishes, and closes the
  // camera and the card on the way out. SIGKILL would leave a participant in
  // the room until LiveKit timed it out, and the viewer watching a frozen
  // picture rather than an ended call.
  ::kill(pid_, SIGTERM);
  timer_.arm_in(kStopGrace);
  log(Level::Info, "media", "asking the call to end");
}

void ScriptMedia::on_timer() {
  if (!stopping_ || pid_ <= 0) {
    return;
  }
  log(Level::Warn, "media", "the call did not end in {}s; killing it", kStopGrace.count());
  ::kill(pid_, SIGKILL);
}

void ScriptMedia::on_child_exit() {
  int status = 0;
  ::waitpid(pid_, &status, 0);  // already exited, so this does not block
  release_child();

  const bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  const bool misconfigured = WIFEXITED(status) && WEXITSTATUS(status) == kConfigExit;
  const bool asked_to_stop = stopping_;
  stopping_ = false;

  if (misconfigured) {
    log(Level::Error, "media",
        "the call cannot start on this device and retrying will not change that - "
        "run {} --check and --dry-run by hand to see which half",
        media_.script.string());
  } else if (!clean) {
    log(Level::Warn, "media", "the call ended badly (status {})", status);
  }

  // A viewer asked for the call moments before the child exited, which means
  // the script had already decided it was finished and never saw them. Telling
  // the core the call is over would leave that viewer watching nothing with
  // nothing left to retry, so start another instead.
  if (!asked_to_stop && !misconfigured && !peers_.empty() &&
      Clock::now() - requested_at_ < kRaceWindow) {
    log(Level::Info, "media", "a viewer arrived as the last call was ending; starting another");
    if (spawn()) {
      return;
    }
  }

  end_for_everyone(clean ? "ended" : "failed");
}

void ScriptMedia::end_for_everyone(const std::string& reason) {
  timer_.disarm();
  // One event per viewer, because the core tracks them one at a time and the
  // process going away is the only thing this side ever learns. A viewer left
  // in that set keeps the LED on Live and stops every clip uploading.
  const std::set<PeerId> ending = std::move(peers_);
  peers_.clear();
  for (const PeerId& peer : ending) {
    sink_(CallEnded{peer, reason});
  }
}

void ScriptMedia::release_child() {
  timer_.disarm();
  if (pidfd_.valid()) {
    reactor_.unwatch(pidfd_.get());
    pidfd_.reset();
  }
  pid_ = -1;
}

}  // namespace porch
