#include "io/gpio_input.h"

#include <gpiod.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <format>
#include <utility>

#include "logging.h"

namespace porch {
namespace {

// One epoll wake-up can cover several edges. Sized for a bounced contact the
// kernel somehow passed through rather than for real presses; whatever is left
// keeps the descriptor readable and comes back on the next turn of the loop.
constexpr std::size_t kEventBatch = 16;

// Both lines are read as "asserted" on a *rising* edge, and the wiring is
// reconciled by active_low rather than by picking a different edge per line.
//
// That is not a guess about the kernel. linux/gpio.h defines the v2 edge flags
// in logical terms - EDGE_RISING is "rising (inactive to active) edges" - and
// ACTIVE_LOW as "line active state is physical low". So with active_low set,
// the button's physical fall to ground, which is the press, arrives as
// GPIOD_EDGE_EVENT_RISING_EDGE. One rule therefore covers a button to ground
// and a PIR driving high, and re-wiring either is a config change rather than
// a code change.
//
// Get this backwards and the doorbell rings on release, which looks like a
// laggy button rather than like a polarity mistake.
gpiod_line_settings* input_settings(bool active_low, gpiod_line_bias bias, int debounce_ms) {
  gpiod_line_settings* settings = gpiod_line_settings_new();
  if (settings == nullptr) {
    return nullptr;
  }
  gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
  gpiod_line_settings_set_active_low(settings, active_low);
  gpiod_line_settings_set_bias(settings, bias);
  // Both edges, though only the rising one becomes an event. The release is
  // what makes a hold measurable, and a line that only ever rises is the
  // signature of a sensor wired the other way up.
  gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_BOTH);
  if (debounce_ms > 0) {
    gpiod_line_settings_set_debounce_period_us(settings,
                                               static_cast<unsigned long>(debounce_ms) * 1000UL);
  }
  return settings;
}

}  // namespace

GpioInput::GpioInput(Reactor& reactor, EventSink sink, const GpioConfig& config)
    : reactor_(reactor),
      sink_(std::move(sink)),
      button_(static_cast<unsigned int>(config.button_line)),
      motion_(static_cast<unsigned int>(config.motion_line)) {
  if (config.button_line < 0 || config.motion_line < 0) {
    throw ConfigError(std::format("gpio: {} and {} are not both line numbers", config.button_line,
                                  config.motion_line));
  }
  if (config.button_line == config.motion_line) {
    throw ConfigError(std::format("gpio: the button and the PIR are both on line {}",
                                  config.button_line));
  }

  gpiod_chip* chip = gpiod_chip_open(config.chip.c_str());
  if (chip == nullptr) {
    throw ConfigError(
        std::format("gpio: cannot open {}: {}. Is this a Pi, and is the user in the gpio group?",
                    config.chip, std::strerror(errno)));
  }

  // The button has no external resistor, so the pull-up that holds it high
  // between presses has to be the Pi's own. The PIR drives its output both
  // ways, so any bias would be fighting it - and a floating line with none is
  // the honest reading of an unplugged sensor rather than a quiet lie.
  gpiod_line_settings* button = input_settings(config.button_active_low, GPIOD_LINE_BIAS_PULL_UP,
                                               config.debounce_ms);
  // No debounce on the PIR: it is a clean digital output, not a contact, and
  // repeat triggers are the core's motion_cooldown_seconds to handle.
  gpiod_line_settings* motion =
      input_settings(config.motion_active_low, GPIOD_LINE_BIAS_DISABLED, 0);
  gpiod_line_config* lines = gpiod_line_config_new();
  gpiod_request_config* wanted = gpiod_request_config_new();

  if (button != nullptr && motion != nullptr && lines != nullptr && wanted != nullptr) {
    // Two settings objects on one config: same request, same descriptor, and
    // different electrical treatment per line.
    if (gpiod_line_config_add_line_settings(lines, &button_, 1, button) == 0 &&
        gpiod_line_config_add_line_settings(lines, &motion_, 1, motion) == 0) {
      gpiod_request_config_set_consumer(wanted, "porchlightd-input");
      request_ = gpiod_chip_request_lines(chip, wanted, lines);
    }
  }

  const int failure = errno;
  gpiod_request_config_free(wanted);
  gpiod_line_config_free(lines);
  gpiod_line_settings_free(motion);
  gpiod_line_settings_free(button);
  gpiod_chip_close(chip);

  if (request_ == nullptr) {
    throw ConfigError(std::format(
        "gpio: cannot watch lines {} and {} on {}: {}. Run `gpioinfo` to see what holds them",
        config.button_line, config.motion_line, config.chip, std::strerror(failure)));
  }

  events_ = gpiod_edge_event_buffer_new(kEventBatch);
  if (events_ == nullptr) {
    gpiod_line_request_release(request_);
    request_ = nullptr;
    throw ConfigError("gpio: cannot allocate an edge-event buffer");
  }
}

GpioInput::~GpioInput() {
  if (watching_) {
    reactor_.unwatch(gpiod_line_request_get_fd(request_));
  }
  if (events_ != nullptr) {
    gpiod_edge_event_buffer_free(events_);
  }
  if (request_ != nullptr) {
    gpiod_line_request_release(request_);
  }
}

void GpioInput::start() {
  reactor_.watch(gpiod_line_request_get_fd(request_), [this] { on_readable(); });
  watching_ = true;
  log(Level::Info, "input", "button on line {}, motion on line {}", button_, motion_);
}

void GpioInput::on_readable() {
  const int got = gpiod_line_request_read_edge_events(request_, events_, kEventBatch);
  if (got < 0) {
    log(Level::Warn, "input", "cannot read edges: {}", std::strerror(errno));
    return;
  }

  for (int i = 0; i < got; ++i) {
    gpiod_edge_event* event =
        gpiod_edge_event_buffer_get_event(events_, static_cast<unsigned long>(i));
    if (event == nullptr) {
      continue;
    }
    handle(gpiod_edge_event_get_line_offset(event),
           gpiod_edge_event_get_event_type(event) == GPIOD_EDGE_EVENT_RISING_EDGE,
           gpiod_edge_event_get_timestamp_ns(event));
  }
}

void GpioInput::handle(unsigned int line, bool asserted, std::uint64_t at_ns) {
  if (line != button_ && line != motion_) {
    return;  // not ours; the kernel only sends what was requested, so this is paranoia
  }
  std::uint64_t& since = (line == button_) ? button_since_ns_ : motion_since_ns_;

  if (asserted) {
    since = at_ns;
    if (line == button_) {
      log(Level::Info, "input", "button pressed");
      sink_(ButtonPressed{});
    } else {
      log(Level::Info, "input", "motion");
      sink_(MotionDetected{});
    }
    return;
  }

  // The release raises nothing - the core has no event for it - but it is the
  // only place the hold is visible, and an AM312's 2-2.5 s reads straight off
  // this line. A press with no release means the wiring is inverted.
  if (since == 0) {
    return;
  }
  const double held = static_cast<double>(at_ns - since) / 1e9;
  since = 0;
  log(Level::Debug, "input", "{} released after {:.2f}s", line == button_ ? "button" : "motion",
      held);
}

}  // namespace porch
