#pragma once

namespace porch {

// Where ButtonPressed and MotionDetected come from. Contact debounce belongs
// here, in milliseconds and in hardware terms - it is not the press de-dup
// window, which is seconds and lives in the core.
//
// Real backend later: libgpiod lines for the button and the PIR.
class InputSource {
 public:
  virtual ~InputSource() = default;
  virtual void start() = 0;
};

}  // namespace porch
