#pragma once

#include "io/chime.h"
#include "logging.h"

namespace porch {

class ConsoleChime : public Chime {
 public:
  void play() override { log(Level::Info, "chime", "ding"); }
};

// Refuses every time, standing in for a sound card held by a live call. The
// point of it is that nothing else changes: no alert is lost and the daemon
// does not stop. Selected with backends.chime = "busy".
class BusyChime : public Chime {
 public:
  void play() override {
    log(Level::Warn, "chime", "device busy, chime not played (see docs/hardware-notes.md)");
  }
};

}  // namespace porch
