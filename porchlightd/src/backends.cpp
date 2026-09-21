#include "backends.h"

#include <format>

#include "io/alsa_chime.h"
#include "io/console_chime.h"
#include "io/console_led.h"
#include "io/fake_recorder.h"
#include "io/gst_recorder.h"
#include "io/logging_server_link.h"
#include "io/logging_uploader.h"
#include "io/stdin_input.h"
#include "io/stub_media.h"

namespace porch {
namespace {

[[noreturn]] void unknown(std::string_view key, const std::string& name) {
  throw ConfigError(std::format("backends.{}: unknown backend '{}'", key, name));
}

std::unique_ptr<Led> make_led(const std::string& name) {
  if (name == "console") {
    return std::make_unique<ConsoleLed>();
  }
  // When the LED arrives:
  //   if (name == "gpio") return std::make_unique<GpioLed>(config.gpio);
  unknown("led", name);
}

std::unique_ptr<Chime> make_chime(const Config& config, Reactor& reactor) {
  const std::string& name = config.backends.chime;
  if (name == "console") {
    return std::make_unique<ConsoleChime>();
  }
  if (name == "busy") {
    return std::make_unique<BusyChime>();
  }
  if (name == "alsa") {
    return std::make_unique<AlsaChime>(reactor, config.chime);
  }
  unknown("chime", name);
}

std::unique_ptr<ClipUploader> make_uploader(const std::string& name, const EventSink& sink) {
  if (name == "log") {
    return std::make_unique<LoggingUploader>(sink);
  }
  unknown("uploader", name);
}

std::unique_ptr<MediaController> make_media(const std::string& name) {
  if (name == "stub") {
    return std::make_unique<StubMedia>();
  }
  unknown("media", name);
}

std::unique_ptr<Recorder> make_recorder(const Config& config, Reactor& reactor,
                                        const EventSink& sink) {
  if (config.backends.recorder == "fake") {
    return std::make_unique<FakeRecorder>(reactor, sink, config.spool.path);
  }
  if (config.backends.recorder == "gstreamer") {
    return std::make_unique<GstRecorder>(reactor, sink, config.recorder, config.spool.path);
  }
  unknown("recorder", config.backends.recorder);
}

}  // namespace

Hardware make_hardware(const Config& config, Reactor& reactor, const EventSink& sink) {
  Hardware hardware;
  hardware.led = make_led(config.backends.led);
  hardware.chime = make_chime(config, reactor);
  hardware.uploader = make_uploader(config.backends.uploader, sink);
  hardware.media = make_media(config.backends.media);
  hardware.recorder = make_recorder(config, reactor, sink);

  // The fake link is the only one whose connectivity can be driven by hand, so
  // the typed "online" and "offline" commands reach it directly rather than
  // through the ServerLink interface, where a real link has no such control.
  StdinInput::LinkControl link = [](bool) {
    log(Level::Warn, "input", "this server backend cannot be taken up or down by hand");
  };

  if (config.backends.server == "log") {
    auto fake = std::make_unique<LoggingServerLink>(config.device_id, sink);
    LoggingServerLink* raw = fake.get();  // owned by hardware.server below
    link = [raw](bool online) { raw->set_online(online); };
    hardware.server = std::move(fake);
  } else {
    unknown("server", config.backends.server);
  }

  if (config.backends.input == "stdin") {
    hardware.input = std::make_unique<StdinInput>(reactor, sink, std::move(link));
  } else {
    // When the button and the PIR arrive:
    //   if (name == "gpio") return std::make_unique<GpioInput>(reactor, sink, config.gpio);
    unknown("input", config.backends.input);
  }

  return hardware;
}

}  // namespace porch
