#pragma once

#include <sys/types.h>

#include "config.h"
#include "io/chime.h"
#include "reactor.h"
#include "unique_fd.h"

namespace porch {

// Plays a sound through the card with aplay, as a child process so the loop
// never waits on it.
//
// Best effort, always, and every failure is a warning rather than an error:
// the file may be missing, aplay may not be installed, and the card is held
// outright whenever a call is live. None of that may stop the daemon or cost
// an alert. See docs/hardware-notes.md.
class AlsaChime : public Chime {
 public:
  AlsaChime(Reactor& reactor, ChimeConfig config);
  ~AlsaChime() override;

  void play() override;

 private:
  void on_child_exit();
  void release();

  Reactor& reactor_;
  ChimeConfig config_;
  pid_t pid_ = -1;
  UniqueFd pidfd_;
};

}  // namespace porch
