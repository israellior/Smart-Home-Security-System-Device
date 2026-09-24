#include "io/gpio_led.h"

#include <gpiod.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <string>

#include "logging.h"

namespace porch {
namespace {

// libgpiod v2 builds a request out of three throwaway objects, any of which can
// fail, and every one has to be freed whichever way the function leaves. The
// ladder is unwound here so that the constructor reads as one call.
//
// Returns nullptr with errno still set by whichever step failed.
gpiod_line_request* request_output_line(const std::string& chip_path, unsigned int offset,
                                        bool active_low) {
  gpiod_line_request* request = nullptr;

  gpiod_chip* chip = gpiod_chip_open(chip_path.c_str());
  if (chip == nullptr) {
    return nullptr;
  }

  gpiod_line_settings* settings = gpiod_line_settings_new();
  gpiod_line_config* lines = gpiod_line_config_new();
  gpiod_request_config* wanted = gpiod_request_config_new();

  if (settings != nullptr && lines != nullptr && wanted != nullptr) {
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_active_low(settings, active_low);
    // Start dark. Without this the line takes whatever the kernel left on it,
    // and the first thing anyone sees of a fresh daemon is a lit porch.
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);

    if (gpiod_line_config_add_line_settings(lines, &offset, 1, settings) == 0) {
      // Whoever holds a line shows up in `gpioinfo` under this name, which is
      // the only way to find out what took it when a second copy cannot start.
      gpiod_request_config_set_consumer(wanted, "porchlightd-led");
      request = gpiod_chip_request_lines(chip, wanted, lines);
    }
  }

  const int failure = errno;
  gpiod_request_config_free(wanted);
  gpiod_line_config_free(lines);
  gpiod_line_settings_free(settings);
  gpiod_chip_close(chip);
  errno = failure;
  return request;
}

}  // namespace

GpioLed::GpioLed(Reactor& reactor, const GpioConfig& config)
    : reactor_(reactor), offset_(static_cast<unsigned int>(config.led_line)) {
  if (config.led_line < 0) {
    throw ConfigError(std::format("gpio.led_line: {} is not a line number", config.led_line));
  }

  request_ = request_output_line(config.chip, offset_, config.led_active_low);
  if (request_ == nullptr) {
    // Naming the escape hatch matters as much as naming the fault: a line held
    // by something else is a wiring or a stale-process problem, and the porch
    // should still be able to answer the door while it is sorted out.
    throw ConfigError(std::format(
        "gpio: cannot drive line {} on {}: {}. Check `gpioinfo`, or set backends.led to "
        "\"console\" to run without the LED",
        config.led_line, config.chip, std::strerror(errno)));
  }

  reactor_.watch(timer_.fd(), [this] { on_timer(); });
  watching_ = true;
  log(Level::Info, "led", "line {} on {}{}", config.led_line, config.chip,
      config.led_active_low ? " (active low)" : "");
}

GpioLed::~GpioLed() {
  if (watching_) {
    reactor_.unwatch(timer_.fd());
  }
  if (request_ != nullptr) {
    // The line keeps its last level once the request is released, so a daemon
    // that stops during a call would otherwise leave the LED on for good.
    drive(false);
    gpiod_line_request_release(request_);
  }
}

void GpioLed::show(LedPattern pattern) {
  phases_ = led_phases(pattern);
  // Deliberately restarts at phase 0 rather than carrying the position over:
  // the core only calls this on a change, and a ring that began half-way
  // through its own dark phase would look like a dropped press.
  enter_phase(0);
}

void GpioLed::enter_phase(std::size_t phase) {
  phase_ = phase;
  drive(phases_[phase_].on);

  if (phases_.size() > 1) {
    timer_.arm_in(phases_[phase_].hold);
  } else {
    timer_.disarm();
  }
}

void GpioLed::on_timer() {
  timer_.drain();
  enter_phase((phase_ + 1) % phases_.size());
}

void GpioLed::drive(bool on) {
  const gpiod_line_value value = on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
  if (gpiod_line_request_set_value(request_, offset_, value) < 0) {
    // Warned, not thrown: this runs on the event loop, and a blink that failed
    // must not take the doorbell down with it.
    log(Level::Warn, "led", "cannot set the line: {}", std::strerror(errno));
  }
}

}  // namespace porch
