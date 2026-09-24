#pragma once

#include <cstddef>
#include <span>

#include "config.h"
#include "io/led.h"
#include "io/led_patterns.h"
#include "reactor.h"
#include "timer_fd.h"

// libgpiod's own type, forward-declared so that <gpiod.h> stays inside the .cpp
// and backends.cpp does not acquire the dependency by including this.
struct gpiod_line_request;

namespace porch {

// One plain single-colour LED on one line, through libgpiod v2. The line
// sources the current directly into a series resistor and on to ground, so
// "active" means the line is high unless led_active_low says otherwise.
//
// The core emits SetLed only when the pattern *changes*, so anything that
// blinks has to be kept going from here. This owns a timer of its own - the
// reactor's single one belongs to the core - and walks the phase table in
// led_patterns.h.
class GpioLed : public Led {
 public:
  GpioLed(Reactor& reactor, const GpioConfig& config);
  ~GpioLed() override;

  void show(LedPattern pattern) override;

 private:
  void enter_phase(std::size_t phase);
  void on_timer();
  void drive(bool on);

  Reactor& reactor_;
  TimerFd timer_;
  gpiod_line_request* request_ = nullptr;
  unsigned int offset_ = 0;
  std::span<const LedPhase> phases_ = led_phases(LedPattern::Off);
  std::size_t phase_ = 0;
  bool watching_ = false;
};

}  // namespace porch
