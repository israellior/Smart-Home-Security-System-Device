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

struct UploadClip {
  EventId event_id;
  std::string path;
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
