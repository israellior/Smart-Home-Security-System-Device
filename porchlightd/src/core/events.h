#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>

#include "core/types.h"

namespace porch {

struct ButtonPressed {};
struct MotionDetected {};

struct ViewerRequested {
  PeerId peer;
};

struct CallEnded {
  PeerId peer;
  std::string reason;
};

struct RecordingFinished {
  EventId event_id;
  std::string path;
  bool ok = false;
  std::chrono::milliseconds duration{0};
  std::uintmax_t bytes = 0;
};

struct UploadFinished {
  EventId event_id;
  bool ok = false;
};

struct AlertResult {
  EventId event_id;
  Kind kind = Kind::Motion;
  AlertOutcome outcome = AlertOutcome::Failed;
};

struct ServerOnline {};

// The link is down. `permanent` says no amount of waiting will fix it - the
// credential was refused, another connection of our role took over, or there
// is no credential on the card at all - so the LED asks for a person instead
// of showing the ordinary offline blink. Everything else about being offline
// is unchanged: alerts still queue, clips still wait, and the chime still
// rings, because none of those need the server's permission.
struct ServerOffline {
  bool permanent = false;
};

// The loop arms one timer at Core::next_deadline() and sends this when it
// fires. There is no timer id: the core keeps its own deadlines, which is what
// lets a test drive every timing rule by moving a fake clock.
struct Tick {};

struct Shutdown {};

using Event = std::variant<ButtonPressed, MotionDetected, ViewerRequested, CallEnded,
                           RecordingFinished, UploadFinished, AlertResult, ServerOnline,
                           ServerOffline, Tick, Shutdown>;

}  // namespace porch
