#pragma once

#include <memory>

#include "config.h"
#include "event_sink.h"
#include "io/chime.h"
#include "io/clip_uploader.h"
#include "io/input_source.h"
#include "io/led.h"
#include "io/media_controller.h"
#include "io/recorder.h"
#include "io/server_link.h"
#include "reactor.h"

namespace porch {

// Everything the daemon talks to the world through. Named Hardware rather than
// Backends because Config already has a block by that name - this is the set
// of objects those config strings select.
struct Hardware {
  std::unique_ptr<InputSource> input;
  std::unique_ptr<Led> led;
  std::unique_ptr<Chime> chime;
  std::unique_ptr<ServerLink> server;
  std::unique_ptr<ClipUploader> uploader;
  std::unique_ptr<MediaController> media;
  std::unique_ptr<Recorder> recorder;
};

// The one place a config string becomes an implementation. Adding a real
// backend means adding a branch here and nothing else: no core change, no test
// change, and the fake stays available to fall back to.
//
// Throws ConfigError naming the key when a backend is not known to this build.
Hardware make_hardware(const Config& config, Reactor& reactor, const EventSink& sink);

}  // namespace porch
