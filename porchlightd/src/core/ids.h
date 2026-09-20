#pragma once

#include "core/types.h"

namespace porch {

// Injected for the same reason the clock is: a UUID is entropy, and a test that
// cannot predict the event id cannot assert that the alert and the clip carry
// the same one.
class IdSource {
 public:
  virtual ~IdSource() = default;
  virtual EventId next() = 0;
};

}  // namespace porch
