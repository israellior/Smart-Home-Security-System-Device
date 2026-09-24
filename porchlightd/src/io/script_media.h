#pragma once

#include <sys/types.h>

#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "core/clock.h"
#include "event_sink.h"
#include "io/media_controller.h"
#include "reactor.h"
#include "timer_fd.h"
#include "unique_fd.h"

namespace porch {

// What the daemon runs to bring up a call. Exported because it is the one part
// of this that can be checked without a camera, a sound card or a network -
// and because one of its properties is a rule rather than a detail: the device
// credential is passed as a *path*, never as a value, or it would be readable
// in `ps` by every user on the machine.
std::vector<std::string> build_media_command(const ServerConfig& server, const MediaConfig& media,
                                             const std::string& device_id);

// pi/webrtc-video.py as a child process, watched through a pidfd.
//
// There is no socket between the two and nothing to say on one. The script
// fetches its own LiveKit tokens with the device credential, joins the room,
// and decides for itself when the call is over - it is the half that can see
// who is in the room, and this daemon is not. So the whole interface is: start
// it, and learn that it stopped.
//
// **One process serves every viewer.** A second viewer-requested while it is
// running is nothing to act on: the Pi publishes one stream and LiveKit copies
// it out, which is the entire reason there is an SFU.
class ScriptMedia : public MediaController {
 public:
  ScriptMedia(Reactor& reactor, EventSink sink, ServerConfig server, MediaConfig media,
              std::string device_id);
  ~ScriptMedia() override;

  void start_call(const PeerId& peer) override;
  void stop_call() override;

 private:
  bool spawn();
  void on_child_exit();
  void on_timer();
  void release_child();
  void end_for_everyone(const std::string& reason);

  Reactor& reactor_;
  EventSink sink_;
  ServerConfig server_;
  MediaConfig media_;
  std::string device_id_;

  TimerFd timer_;
  // Every viewer this process was started for. They all end together, because
  // the process ending is the only thing this side can observe.
  std::set<PeerId> peers_;
  // When a viewer was last asked for. A child that exits just after one had
  // already decided to go, so the request never reached it: see on_child_exit.
  TimePoint requested_at_{};
  pid_t pid_ = -1;
  UniqueFd pidfd_;
  bool stopping_ = false;
};

}  // namespace porch
