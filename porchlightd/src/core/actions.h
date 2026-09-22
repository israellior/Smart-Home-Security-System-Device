#pragma once

#include <chrono>
#include <string>
#include <variant>

#include "core/clock.h"
#include "core/types.h"

namespace porch {

struct SetLed {
  LedPattern pattern = LedPattern::Off;
};

struct PlayChime {};

struct SendAlert {
  EventId event_id;
  Kind kind = Kind::Motion;
  // When the sensor fired, not when this was emitted: an alert held through an
  // outage still reports the time of the event. Steady, so the server link
  // converts it to wall-clock as it sends.
  TimePoint triggered_at;
};

struct StartRecording {
  EventId event_id;
  std::chrono::seconds seconds{0};
};

struct StopRecording {};

// Everything the confirm at the end of the upload has to state, because only
// the core knows any of it once the recorder has let go of the file. The
// uploader writes it beside the clip as a sidecar and then sends it; see
// docs/protocol.md.
struct UploadClip {
  EventId event_id;
  std::string path;
  Kind kind = Kind::Motion;
  // As on SendAlert: when the sensor fired, steady, converted to wall clock as
  // it goes out. A clip uploaded after an hour offline still says when the
  // person was at the door.
  TimePoint triggered_at;
  std::chrono::milliseconds duration{0};
  // The clip is shorter than the length that was asked for, because something
  // stopped it early - a viewer arriving, or the daemon shutting down. It is
  // playable either way, and a UI should say so.
  bool partial = false;
};

// A clip that will never be uploaded: the recorder failed, or a viewer cut it
// so short there is nothing worth keeping. Someone still has to delete it.
struct DiscardClip {
  EventId event_id;
  std::string path;
};

struct StartCall {
  PeerId peer;
};

struct StopCall {};

using Action = std::variant<SetLed, PlayChime, SendAlert, StartRecording, StopRecording, UploadClip,
                            DiscardClip, StartCall, StopCall>;

}  // namespace porch
