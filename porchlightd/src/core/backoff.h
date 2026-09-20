#pragma once

#include <algorithm>
#include <chrono>

#include "core/clock.h"

namespace porch {

// Exponential from an initial delay to a cap, reset by a success.
class Backoff {
 public:
  Backoff(std::chrono::seconds initial, std::chrono::seconds cap)
      : initial_(initial), cap_(cap), next_(initial) {}

  void reset() { next_ = initial_; }

  // Returns the earliest time the next attempt may be made.
  TimePoint fail(TimePoint now) {
    const TimePoint retry_at = now + next_;
    next_ = std::min(next_ * 2, cap_);
    return retry_at;
  }

 private:
  std::chrono::seconds initial_;
  std::chrono::seconds cap_;
  std::chrono::seconds next_;
};

}  // namespace porch
