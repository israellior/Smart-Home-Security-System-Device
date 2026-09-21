#pragma once

#include "io/led.h"
#include "logging.h"

namespace porch {

class ConsoleLed : public Led {
 public:
  void show(LedPattern pattern) override { log(Level::Info, "led", "{}", to_string(pattern)); }
};

}  // namespace porch
