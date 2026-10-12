#include "backends.h"

#include <format>

namespace porch {
namespace {

[[noreturn]] void unknown(std::string_view key, const std::string& name) {
  throw ConfigError(std::format("backends.{}: unknown backend '{}'", key, name));
}

void expect(std::string_view key, const std::string& name, std::string_view only) {
  if (name != only) {
    unknown(key, name);
  }
}

}  // namespace

Hardware make_hardware(const Config& config, Reactor& reactor, const EventSink& sink) {
  expect("led", config.backends.led, "gpio");
  expect("chime", config.backends.chime, "alsa");
  expect("uploader", config.backends.uploader, "script");
  expect("media", config.backends.media, "script");
  expect("recorder", config.backends.recorder, "gstreamer");
  expect("server", config.backends.server, "bridge");
  expect("input", config.backends.input, "gpio");

  Hardware hardware;
  hardware.led = std::make_unique<GpioLed>(reactor, config.gpio);
  hardware.chime = std::make_unique<AlsaChime>(reactor, config.chime);
  hardware.uploader =
      std::make_unique<ScriptUploader>(reactor, sink, config.server, config.device_id);
  hardware.media = std::make_unique<ScriptMedia>(reactor, sink, config.server, config.media,
                                                 config.device_id);
  hardware.recorder =
      std::make_unique<GstRecorder>(reactor, sink, config.recorder, config.spool.path);
  hardware.server =
      std::make_unique<BridgeServerLink>(reactor, sink, config.server, config.device_id);
  hardware.input = std::make_unique<GpioInput>(reactor, sink, config.gpio);
  return hardware;
}

}  // namespace porch
