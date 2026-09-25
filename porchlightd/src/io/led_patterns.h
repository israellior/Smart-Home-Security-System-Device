#pragma once

#include <chrono>
#include <span>

#include "core/types.h"

namespace porch {

// One step of a blink: hold the LED at `on` for `hold`, then take the next and
// wrap at the end. A pattern of one phase is static, and `hold` is unread
// there - the backend disarms its timer rather than waking up forever to set
// the level it is already at.
struct LedPhase {
  bool on;
  std::chrono::milliseconds hold;
};

// One colour and no PWM: a plain GPIO line is on or off, so "pulse" has to be
// a blink and the seven patterns can only be told apart by rate and grouping.
// They are deliberately far apart rather than evenly spread - Ring at 4 Hz
// against Recording at 0.5 Hz is a difference that survives being read across
// a dark porch, and Offline is a shape rather than a rate so that "the server
// is unreachable" cannot be mistaken for a slower version of anything else.
//
// Kept out of gpio_led.cpp, and free of libgpiod, so that the table is checked
// by the tests on a machine with no GPIO - which is every machine here.
inline std::span<const LedPhase> led_phases(LedPattern pattern) {
  using namespace std::chrono_literals;

  static constexpr LedPhase kRing[] = {{true, 125ms}, {false, 125ms}};
  static constexpr LedPhase kLive[] = {{true, 0ms}};
  static constexpr LedPhase kRecording[] = {{true, 1000ms}, {false, 1000ms}};
  static constexpr LedPhase kOffline[] = {
      {true, 120ms}, {false, 120ms}, {true, 120ms}, {false, 1640ms}};
  // Offline's negative, phase for phase: lit almost all of the time, with the
  // same double interruption cut out of it. Two states that both mean "the
  // server is not hearing us" are the pair most likely to be confused, so they
  // are deliberately opposite rather than merely different - dark with two
  // flashes is waiting for the network, lit with two gaps is waiting for a
  // person. It begins lit because it is not going to end on its own.
  static constexpr LedPhase kFault[] = {
      {true, 1640ms}, {false, 120ms}, {true, 120ms}, {false, 120ms}};
  static constexpr LedPhase kDark[] = {{false, 0ms}};

  switch (pattern) {
    case LedPattern::Ring: return kRing;
    case LedPattern::Live: return kLive;
    case LedPattern::Recording: return kRecording;
    case LedPattern::Fault: return kFault;
    case LedPattern::Offline: return kOffline;
    case LedPattern::Idle: return kDark;
    case LedPattern::Off: return kDark;
  }
  return kDark;
}

}  // namespace porch
