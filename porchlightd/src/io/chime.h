#pragma once

namespace porch {

// Best effort, always. A chime that cannot be played is a warning and never an
// error: the sound card may be held by a live call, and the alert reaches the
// server by a different path regardless. See docs/hardware-notes.md.
class Chime {
 public:
  virtual ~Chime() = default;
  virtual void play() = 0;
};

}  // namespace porch
