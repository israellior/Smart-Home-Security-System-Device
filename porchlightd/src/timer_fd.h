#pragma once

#include <chrono>

#include "core/clock.h"
#include "unique_fd.h"

namespace porch {

// A deadline as a file descriptor, so waiting for one looks like waiting for
// anything else. The reactor owns one for the core; a backend that needs its
// own timing owns another.
class TimerFd {
 public:
  TimerFd();

  int fd() const { return fd_.get(); }

  void arm_at(TimePoint deadline);
  void arm_in(std::chrono::nanoseconds delay);
  void disarm();

  // Level-triggered: an unread expiry would spin the loop.
  void drain();

 private:
  UniqueFd fd_;
};

}  // namespace porch
