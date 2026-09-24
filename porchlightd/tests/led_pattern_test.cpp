// The blink table, which is the whole of what the LED backend decides. Every
// assertion here is about being able to *read* the porch at a glance: one
// colour and six states means the patterns have to be told apart by rate and
// shape alone, and a table typo that makes two of them alike is invisible in
// code review and obvious only at 11pm on a doorstep.
//
// led_patterns.h is deliberately free of libgpiod so that this runs on the
// development machine, which has no GPIO and never will.

#include "io/led_patterns.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace porch {
namespace {

using std::chrono::milliseconds;

const std::vector<LedPattern> kAllPatterns = {LedPattern::Ring,    LedPattern::Live,
                                              LedPattern::Recording, LedPattern::Offline,
                                              LedPattern::Idle,    LedPattern::Off};

milliseconds period(LedPattern pattern) {
  milliseconds total{0};
  for (const LedPhase& phase : led_phases(pattern)) {
    total += phase.hold;
  }
  return total;
}

milliseconds lit(LedPattern pattern) {
  milliseconds total{0};
  for (const LedPhase& phase : led_phases(pattern)) {
    if (phase.on) {
      total += phase.hold;
    }
  }
  return total;
}

bool blinks(LedPattern pattern) { return led_phases(pattern).size() > 1; }

TEST(LedPatterns, EveryPatternHasAPhase) {
  // A missing switch case would return the fallback rather than crash, so the
  // LED would sit dark and nothing would say why.
  for (const LedPattern pattern : kAllPatterns) {
    EXPECT_FALSE(led_phases(pattern).empty()) << to_string(pattern);
  }
}

TEST(LedPatterns, StaticPatternsAreTheOnesWithNothingToTime) {
  EXPECT_FALSE(blinks(LedPattern::Live));
  EXPECT_FALSE(blinks(LedPattern::Idle));
  EXPECT_FALSE(blinks(LedPattern::Off));

  EXPECT_TRUE(blinks(LedPattern::Ring));
  EXPECT_TRUE(blinks(LedPattern::Recording));
  EXPECT_TRUE(blinks(LedPattern::Offline));
}

TEST(LedPatterns, LiveIsSolidAndTheDarkOnesAreDark) {
  ASSERT_EQ(led_phases(LedPattern::Live).size(), 1u);
  EXPECT_TRUE(led_phases(LedPattern::Live)[0].on);

  ASSERT_EQ(led_phases(LedPattern::Idle).size(), 1u);
  EXPECT_FALSE(led_phases(LedPattern::Idle)[0].on);

  ASSERT_EQ(led_phases(LedPattern::Off).size(), 1u);
  EXPECT_FALSE(led_phases(LedPattern::Off)[0].on);
}

TEST(LedPatterns, EveryBlinkStartsLit) {
  // show() restarts at phase 0 on every change, so a pattern beginning dark
  // would make the LED pause at the very moment the state changed - which
  // reads as a missed press rather than as a new pattern.
  for (const LedPattern pattern : kAllPatterns) {
    if (blinks(pattern)) {
      EXPECT_TRUE(led_phases(pattern)[0].on) << to_string(pattern);
    }
  }
}

TEST(LedPatterns, BlinkPhasesAlternateAndWrapCleanly) {
  for (const LedPattern pattern : kAllPatterns) {
    const auto phases = led_phases(pattern);
    if (!blinks(pattern)) {
      continue;
    }
    for (std::size_t i = 1; i < phases.size(); ++i) {
      // Two neighbours at the same level are one longer phase written twice -
      // harmless to the eye, but it means the table says something other than
      // what its author read.
      EXPECT_NE(phases[i].on, phases[i - 1].on) << to_string(pattern) << " phase " << i;
    }
    // The table repeats, so the last phase sits next to the first.
    EXPECT_NE(phases.front().on, phases.back().on) << to_string(pattern) << " wrapping";
  }
}

TEST(LedPatterns, EveryBlinkPhaseLastsLongEnoughToSee) {
  for (const LedPattern pattern : kAllPatterns) {
    if (!blinks(pattern)) {
      continue;
    }
    for (const LedPhase& phase : led_phases(pattern)) {
      // Below about 50ms a blink smears into a dim glow, and the timer wakes
      // the loop for nothing.
      EXPECT_GE(phase.hold, milliseconds{50}) << to_string(pattern);
    }
  }
}

TEST(LedPatterns, RingIsUnmistakablyFasterThanRecording) {
  // The two that are most costly to confuse: someone is at the door, against
  // the camera is running. A factor of four is the point of the table.
  EXPECT_LE(period(LedPattern::Ring) * 4, period(LedPattern::Recording));
}

TEST(LedPatterns, OfflineIsAShapeRatherThanARate) {
  // Two quick flashes in a long gap. Distinguished from Ring and Recording by
  // duty cycle, not speed, so it cannot be misread as either at a distance.
  EXPECT_EQ(led_phases(LedPattern::Offline).size(), 4u);
  EXPECT_LT(lit(LedPattern::Offline) * 4, period(LedPattern::Offline));
}

}  // namespace
}  // namespace porch
