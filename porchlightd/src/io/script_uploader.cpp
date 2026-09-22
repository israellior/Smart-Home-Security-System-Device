#include "io/script_uploader.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

#include "child_process.h"
#include "iso8601.h"
#include "logging.h"

namespace porch {
namespace {

using nlohmann::json;

void remove_quietly(const std::filesystem::path& path) {
  if (path.empty()) {
    return;
  }
  std::error_code ec;
  if (!std::filesystem::remove(path, ec) && ec) {
    // Not fatal on its own, but it is how an SD card fills up, so it is said
    // out loud rather than swallowed.
    log(Level::Warn, "upload", "cannot remove {}: {}", path.string(), ec.message());
  }
}

}  // namespace

std::filesystem::path sidecar_for(const std::filesystem::path& clip) {
  std::filesystem::path sidecar = clip;
  sidecar.replace_extension(".json");
  return sidecar;
}

ScriptUploader::ScriptUploader(Reactor& reactor, EventSink sink, ServerConfig config,
                               std::string device_id)
    : reactor_(reactor),
      sink_(std::move(sink)),
      config_(std::move(config)),
      device_id_(std::move(device_id)) {}

ScriptUploader::~ScriptUploader() {
  if (pid_ > 0) {
    // Killed mid-transfer means no confirm, so the clip and its sidecar are
    // still on the card and the next run sends them again. Nothing is lost by
    // being abrupt here.
    ::kill(pid_, SIGTERM);
    int status = 0;
    ::waitpid(pid_, &status, 0);
  }
  release_child();
}

void ScriptUploader::upload(const UploadClip& clip) {
  if (pid_ > 0) {
    // The core sends one at a time. If that ever changed, answering for a clip
    // that was never started would drop the one that is running out of the
    // queue, so it is refused rather than queued here.
    log(Level::Error, "upload", "asked for {} while {} is still going", clip.event_id, event_id_);
    sink_(UploadFinished{clip.event_id, false});
    return;
  }

  event_id_ = clip.event_id;
  clip_ = clip.path;

  if (!write_sidecar(clip)) {
    finish(false);
    return;
  }

  const pid_t pid = spawn_child({
      config_.uploader_path.string(),
      "--url", config_.base_url,
      "--credential-file", config_.credential_path.string(),
      "--clip", clip_.string(),
  });
  if (pid < 0) {
    log(Level::Error, "upload", "cannot run {}: {}", config_.uploader_path.string(),
        std::strerror(errno));
    finish(false);
    return;
  }

  pid_ = pid;
  pidfd_ = watch_child(pid_);
  if (!pidfd_.valid()) {
    log(Level::Error, "upload", "pidfd_open failed: {}", std::strerror(errno));
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    finish(false);
    return;
  }

  // The child exiting is just another readable descriptor, and its exit code is
  // the entire answer: the three steps, the retries within them and the
  // decision about what counts as done all live in the script.
  reactor_.watch(pidfd_.get(), [this] { on_child_exit(); });
  log(Level::Info, "upload", "sending {} to {}", clip_.filename().string(), config_.base_url);
}

void ScriptUploader::discard(const EventId& event_id, const std::string& path) {
  log(Level::Info, "upload", "discarding {}", event_id);
  remove_quietly(path);
  remove_quietly(sidecar_for(path));
}

bool ScriptUploader::write_sidecar(const UploadClip& clip) {
  const std::filesystem::path sidecar = sidecar_for(clip_);
  const std::filesystem::path partial = sidecar.string() + ".tmp";

  // This is the confirm body, and the only copy of it: the core has moved on by
  // the time the script runs. bytes is left out because the script measures the
  // file itself, and eventId because the script strips it back out - it is here
  // so that a clip found on disk can still say which event it belongs to.
  const json body{
      {"eventId", clip.event_id},
      {"deviceId", device_id_},
      {"kind", to_string(clip.kind)},
      {"at", iso8601_of(clip.triggered_at)},
      {"durationMs", clip.duration.count()},
      {"partial", clip.partial},
  };

  std::ofstream out(partial, std::ios::binary | std::ios::trunc);
  out << body.dump(2) << "\n";
  out.close();
  if (!out) {
    log(Level::Error, "upload", "cannot write {}", partial.string());
    remove_quietly(partial);
    return false;
  }

  // Written aside and renamed, because a half-written sidecar found after a
  // power cut would describe the clip wrongly rather than obviously not at all.
  std::error_code ec;
  std::filesystem::rename(partial, sidecar, ec);
  if (ec) {
    log(Level::Error, "upload", "cannot place {}: {}", sidecar.string(), ec.message());
    remove_quietly(partial);
    return false;
  }
  return true;
}

void ScriptUploader::on_child_exit() {
  int status = 0;
  ::waitpid(pid_, &status, 0);  // already exited, so this does not block
  release_child();

  // Exit 0 is the confirm, never the PUT. The script's own account of which
  // step failed is on stderr and in the journal; this says what it means for
  // the clip, which is that it stays where it is.
  const bool confirmed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (confirmed) {
    log(Level::Info, "upload", "confirmed {}", event_id_);
  } else {
    log(Level::Warn, "upload", "{} not confirmed (status {}); keeping {}", event_id_, status,
        clip_.string());
  }
  finish(confirmed);
}

void ScriptUploader::finish(bool ok) {
  const EventId id = event_id_;
  if (ok) {
    // The one place anything is deleted after a success, and it takes a
    // confirmed upload to get here.
    remove_quietly(clip_);
    remove_quietly(sidecar_for(clip_));
  }
  // Cleared before the sink, which may hand back the next clip synchronously.
  event_id_.clear();
  clip_.clear();
  sink_(UploadFinished{id, ok});
}

void ScriptUploader::release_child() {
  if (pidfd_.valid()) {
    reactor_.unwatch(pidfd_.get());
    pidfd_.reset();
  }
  pid_ = -1;
}

}  // namespace porch
