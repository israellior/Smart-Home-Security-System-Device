#pragma once

#include "core/types.h"

namespace porch {

// The live call, which is a separate program: webrtc-video.py keeps its own
// WebSocket and its own pipeline. This daemon only asks it to start and stop.
//
// Real backend later: a local Unix socket to that script, which needs a small
// change to it and is out of scope for now. Until then CallEnded arrives from
// the fake input instead.
class MediaController {
 public:
  virtual ~MediaController() = default;
  virtual void start_call(const PeerId& peer) = 0;
  virtual void stop_call() = 0;
};

}  // namespace porch
