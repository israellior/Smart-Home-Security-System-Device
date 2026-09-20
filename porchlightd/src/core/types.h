#pragma once

#include <ostream>
#include <string>

namespace porch {

using EventId = std::string;
using PeerId = std::string;

enum class Kind { Motion, Ring };

// In priority order as the core resolves them, highest first. A single
// enumerator has to stand for the whole device state, so the order here is the
// rule: a ring outranks a live call, which outranks a recording.
enum class LedPattern { Ring, Live, Recording, Offline, Idle, Off };

// What the server said about an alert, which is not the same question as
// whether the send worked. A rejection is permanent and must not be retried; a
// failure is the link, and must be.
enum class AlertOutcome { Delivered, Rejected, Failed };

inline const char* to_string(Kind kind) {
  switch (kind) {
    case Kind::Motion: return "motion";
    case Kind::Ring: return "ring";
  }
  return "?";
}

inline const char* to_string(LedPattern pattern) {
  switch (pattern) {
    case LedPattern::Ring: return "ring";
    case LedPattern::Live: return "live";
    case LedPattern::Recording: return "recording";
    case LedPattern::Offline: return "offline";
    case LedPattern::Idle: return "idle";
    case LedPattern::Off: return "off";
  }
  return "?";
}

inline const char* to_string(AlertOutcome outcome) {
  switch (outcome) {
    case AlertOutcome::Delivered: return "delivered";
    case AlertOutcome::Rejected: return "rejected";
    case AlertOutcome::Failed: return "failed";
  }
  return "?";
}

// So a failing test prints "ring" rather than an integer.
inline std::ostream& operator<<(std::ostream& out, Kind kind) { return out << to_string(kind); }
inline std::ostream& operator<<(std::ostream& out, LedPattern p) { return out << to_string(p); }
inline std::ostream& operator<<(std::ostream& out, AlertOutcome o) { return out << to_string(o); }

}  // namespace porch
