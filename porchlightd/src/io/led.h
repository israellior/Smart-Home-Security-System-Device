#pragma once

#include "core/types.h"

namespace porch {

// Real backend later: a GPIO line driven through libgpiod.
class Led {
 public:
  virtual ~Led() = default;
  virtual void show(LedPattern pattern) = 0;
};

}  // namespace porch
