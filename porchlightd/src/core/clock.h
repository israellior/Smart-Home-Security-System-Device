#pragma once

#include <chrono>

namespace porch {

// Deadlines are steady: a wall-clock correction must never shorten a cooldown
// or fire a retry early. The one wall-clock value the system needs is the
// timestamp on an alert, and the server link derives that from the steady
// timestamp when it sends.
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

}  // namespace porch
