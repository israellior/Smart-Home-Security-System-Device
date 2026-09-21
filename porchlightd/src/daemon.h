#pragma once

#include <deque>

#include "backends.h"
#include "config.h"
#include "core/core.h"
#include "random_ids.h"
#include "reactor.h"

namespace porch {

// Joins the three parts: the core decides, the backends act, the reactor
// waits. It holds no rules of its own - anything that looks like a decision
// here belongs in the core instead.
class Daemon {
 public:
  Daemon(const Config& config, Reactor& reactor);

  void run();

 private:
  void deliver(Event event);
  void drain();
  void execute(const Action& action);

  Reactor& reactor_;
  RandomIds ids_;
  Core core_;
  Hardware hardware_;

  // Events are queued, never handled where they arise: a backend reporting a
  // result from inside an action it is still carrying out would otherwise
  // re-enter the core mid-decision.
  std::deque<Event> pending_;
  bool draining_ = false;
  bool stopping_ = false;
};

}  // namespace porch
