#pragma once

#include "io/media_controller.h"
#include "logging.h"

namespace porch {

// CallEnded does not appear here: with no link to webrtc-video.py yet, only
// the fake input can say a call is over.
class StubMedia : public MediaController {
 public:
  void start_call(const PeerId& peer) override {
    log(Level::Info, "media", "would start call with peer={}", peer);
  }

  void stop_call() override { log(Level::Info, "media", "would stop the call"); }
};

}  // namespace porch
