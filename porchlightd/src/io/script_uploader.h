#pragma once

#include <sys/types.h>

#include <filesystem>
#include <string>

#include "config.h"
#include "core/actions.h"
#include "core/types.h"
#include "event_sink.h"
#include "reactor.h"
#include "unique_fd.h"

namespace porch {

// Where the description of a clip lives while it waits: <eventId>.json beside
// <eventId>.mp4. It is written before the upload starts and removed with the
// clip, and it is exactly the body the confirm sends.
std::filesystem::path sidecar_for(const std::filesystem::path& clip);

// pi/upload-clip.py as a child process, watched through a pidfd and judged
// entirely by its exit code: 0 means the confirm succeeded, and only then may
// the local copy go.
//
// A separate process for the same reason as the bridge - the three steps are
// HTTPS with a signed URL in the middle, and this daemon has no HTTP client and
// no business growing one - but a separate process from the bridge as well. An
// upload is minutes of traffic to a bucket; the socket carrying alerts must not
// wait behind it or die with it.
//
// It deletes a clip after a successful upload, which is why discarding one is
// its job too rather than a separate concern.
class ScriptUploader {
 public:
  ScriptUploader(Reactor& reactor, EventSink sink, ServerConfig config, std::string device_id);
  ~ScriptUploader();

  // Takes the whole action rather than a path: the confirm at the end of the
  // upload has to state the kind, the trigger time, the duration and whether a
  // viewer cut it short, and the core is the only thing that knows them.
  //
  // Always answered with an UploadFinished event, or the clip is never retried.
  void upload(const UploadClip& clip);

  // Nothing comes back for this one: the clip is simply gone.
  void discard(const EventId& event_id, const std::string& path);

 private:
  void on_child_exit();
  void finish(bool ok);
  void release_child();
  bool write_sidecar(const UploadClip& clip);

  Reactor& reactor_;
  EventSink sink_;
  ServerConfig config_;
  std::string device_id_;

  EventId event_id_;
  std::filesystem::path clip_;
  pid_t pid_ = -1;
  UniqueFd pidfd_;
};

}  // namespace porch
