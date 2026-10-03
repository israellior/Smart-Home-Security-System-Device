#include "backends.h"

#include <format>

#include "io/alsa_chime.h"
#include "io/bridge_server_link.h"
#ifdef PORCHLIGHT_GPIO
#include "io/gpio_input.h"
#include "io/gpio_led.h"
#endif
#include "io/gst_recorder.h"
#include "io/script_media.h"
#include "io/script_uploader.h"

namespace porch {
namespace {

[[noreturn]] void unknown(std::string_view key, const std::string& name) {
  throw ConfigError(std::format("backends.{}: unknown backend '{}'", key, name));
}

#ifndef PORCHLIGHT_GPIO
// Distinct from unknown() on purpose. "gpio" is a real backend that this binary
// simply does not contain, and reading that as a typo costs an hour.
[[noreturn]] void not_built_in(std::string_view key) {
  throw ConfigError(std::format(
      "backends.{}: \"gpio\" needs a build with -DPORCHLIGHT_GPIO=ON, and libgpiod 2.x "
      "(apt install libgpiod-dev) to build it against",
      key));
}
#endif

std::unique_ptr<Led> make_led(const Config& config, [[maybe_unused]] Reactor& reactor) {
  const std::string& name = config.backends.led;
  if (name == "gpio") {
#ifdef PORCHLIGHT_GPIO
    return std::make_unique<GpioLed>(reactor, config.gpio);
#else
    not_built_in("led");
#endif
  }
  unknown("led", name);
}

std::unique_ptr<Chime> make_chime(const Config& config, Reactor& reactor) {
  const std::string& name = config.backends.chime;
  if (name == "alsa") {
    return std::make_unique<AlsaChime>(reactor, config.chime);
  }
  unknown("chime", name);
}

std::unique_ptr<ClipUploader> make_uploader(const Config& config, Reactor& reactor,
                                            const EventSink& sink) {
  const std::string& name = config.backends.uploader;
  if (name == "script") {
    return std::make_unique<ScriptUploader>(reactor, sink, config.server, config.device_id);
  }
  unknown("uploader", name);
}

std::unique_ptr<MediaController> make_media(const Config& config, Reactor& reactor,
                                            const EventSink& sink) {
  const std::string& name = config.backends.media;
  if (name == "script") {
    return std::make_unique<ScriptMedia>(reactor, sink, config.server, config.media,
                                         config.device_id);
  }
  unknown("media", name);
}

std::unique_ptr<Recorder> make_recorder(const Config& config, Reactor& reactor,
                                        const EventSink& sink) {
  if (config.backends.recorder == "gstreamer") {
    return std::make_unique<GstRecorder>(reactor, sink, config.recorder, config.spool.path);
  }
  unknown("recorder", config.backends.recorder);
}

std::unique_ptr<ServerLink> make_server(const Config& config, Reactor& reactor,
                                        const EventSink& sink) {
  if (config.backends.server == "bridge") {
    return std::make_unique<BridgeServerLink>(reactor, sink, config.server, config.device_id);
  }
  unknown("server", config.backends.server);
}

std::unique_ptr<InputSource> make_input(const Config& config, [[maybe_unused]] Reactor& reactor,
                                        [[maybe_unused]] const EventSink& sink) {
  if (config.backends.input == "gpio") {
#ifdef PORCHLIGHT_GPIO
    return std::make_unique<GpioInput>(reactor, sink, config.gpio);
#else
    not_built_in("input");
#endif
  }
  unknown("input", config.backends.input);
}

}  // namespace

Hardware make_hardware(const Config& config, Reactor& reactor, const EventSink& sink) {
  Hardware hardware;
  hardware.led = make_led(config, reactor);
  hardware.chime = make_chime(config, reactor);
  hardware.uploader = make_uploader(config, reactor, sink);
  hardware.media = make_media(config, reactor, sink);
  hardware.recorder = make_recorder(config, reactor, sink);
  hardware.server = make_server(config, reactor, sink);
  hardware.input = make_input(config, reactor, sink);
  return hardware;
}

}  // namespace porch
