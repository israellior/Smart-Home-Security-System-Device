#pragma once

#include <memory>

#include "config.h"
#include "event_sink.h"
#include "io/alsa_chime.h"
#include "io/bridge_server_link.h"
#include "io/gpio_input.h"
#include "io/gpio_led.h"
#include "io/gst_recorder.h"
#include "io/script_media.h"
#include "io/script_uploader.h"
#include "reactor.h"

namespace porch {

// Everything the daemon talks to the world through. Named Hardware rather than
// Backends because Config already has a block by that name.
//
// Each is the one real implementation, named directly: there is no interface
// in front of any of them. The GPIO headers forward-declare libgpiod's types,
// so this compiles without libgpiod - but only a -DPORCHLIGHT_GPIO=ON build
// links, which is why the porchlightd binary exists only in that build.
struct Hardware {
  std::unique_ptr<GpioInput> input;
  std::unique_ptr<GpioLed> led;
  std::unique_ptr<AlsaChime> chime;
  std::unique_ptr<BridgeServerLink> server;
  std::unique_ptr<ScriptUploader> uploader;
  std::unique_ptr<ScriptMedia> media;
  std::unique_ptr<GstRecorder> recorder;
};

// Throws ConfigError naming the key when a backends.* string is not the one
// this daemon has, so a typo in the config is a startup error and never a
// silent default.
Hardware make_hardware(const Config& config, Reactor& reactor, const EventSink& sink);

}  // namespace porch
