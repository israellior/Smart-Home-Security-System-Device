#include "io/alsa_chime.h"

#include <signal.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include "child_process.h"
#include "logging.h"

namespace porch {

AlsaChime::AlsaChime(Reactor& reactor, ChimeConfig config)
    : reactor_(reactor), config_(std::move(config)) {
  std::error_code ec;
  if (!std::filesystem::exists(config_.sound, ec)) {
    // Said once at startup rather than on every press, and a warning rather
    // than a refusal to start: a doorbell with no chime still answers the door.
    log(Level::Warn, "chime", "{} is missing; presses will be silent",
        config_.sound.string());
  }
}

AlsaChime::~AlsaChime() {
  if (pid_ > 0) {
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
  }
  release();
}

void AlsaChime::play() {
  // A press during a press restarts the sound. Every press has to ring, and
  // two aplays cannot share the device anyway.
  if (pid_ > 0) {
    ::kill(pid_, SIGTERM);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    release();
  }

  const pid_t pid = spawn_child({"aplay", "-q", "-D", config_.device, config_.sound.string()});
  if (pid < 0) {
    log(Level::Warn, "chime", "cannot run aplay: {}", std::strerror(errno));
    return;
  }

  UniqueFd watcher = watch_child(pid);
  if (!watcher.valid()) {
    log(Level::Warn, "chime", "pidfd_open failed: {}", std::strerror(errno));
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return;
  }

  pid_ = pid;
  pidfd_ = std::move(watcher);
  reactor_.watch(pidfd_.get(), [this] { on_child_exit(); });
  log(Level::Info, "chime", "ding");
}

void AlsaChime::on_child_exit() {
  int status = 0;
  ::waitpid(pid_, &status, 0);
  const bool played = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  release();

  if (!played) {
    // aplay has already printed the reason on the line above, so this does not
    // guess at one. A card held by a live call and a missing file look alike
    // from here, and neither is fatal or costs an alert.
    if (WIFEXITED(status)) {
      log(Level::Warn, "chime", "aplay exited {}; its own message is above", WEXITSTATUS(status));
    } else {
      log(Level::Warn, "chime", "aplay was killed by signal {}", WTERMSIG(status));
    }
  }
}

void AlsaChime::release() {
  if (pidfd_.valid()) {
    reactor_.unwatch(pidfd_.get());
    pidfd_.reset();
  }
  pid_ = -1;
}

}  // namespace porch
