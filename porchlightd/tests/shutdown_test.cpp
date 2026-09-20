#include <gtest/gtest.h>

#include "support.h"

namespace porch::test {
namespace {

TEST(Shutdown, StopsTheRecorderTheCallAndTheLight) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  h.after(2s, ViewerRequested{"viewer-1"});
  h.after(1s, good_clip("evt-1", 3s));  // the call is live, nothing is recording
  h.send(MotionDetected{});             // alerts, but cannot record during a call

  const auto out = h.send(Shutdown{});

  EXPECT_EQ(names(out), (std::vector<std::string>{"StopCall", "SetLed"}));
  EXPECT_EQ(find<SetLed>(out)->pattern, LedPattern::Off);
}

TEST(Shutdown, StopsARecordingThatIsStillRunning) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});

  const auto out = h.after(3s, Shutdown{});

  EXPECT_EQ(names(out), (std::vector<std::string>{"StopRecording", "SetLed"}));
}

TEST(Shutdown, TurnsTheLightOffEvenWithNothingRunning) {
  Harness h;
  h.go_online();

  const auto out = h.send(Shutdown{});

  EXPECT_EQ(names(out), (std::vector<std::string>{"SetLed"}));
  EXPECT_EQ(find<SetLed>(out)->pattern, LedPattern::Off);
}

TEST(Shutdown, AsksForNoFurtherWakeups) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  ASSERT_TRUE(h.core().next_deadline().has_value());

  h.send(Shutdown{});

  EXPECT_FALSE(h.core().next_deadline().has_value());
}

TEST(Shutdown, IgnoresTriggersAfterwards) {
  Harness h;
  h.go_online();
  h.send(Shutdown{});

  const auto out = h.send(ButtonPressed{});

  EXPECT_TRUE(out.empty());
}

TEST(Deadline, IsTheEarliestOfThePendingOnes) {
  Harness h;
  h.go_online();
  const TimePoint start = h.now();

  h.send(MotionDetected{});

  // The cooldown at twenty seconds beats the alert ageing out at sixty.
  ASSERT_TRUE(h.core().next_deadline().has_value());
  EXPECT_EQ(*h.core().next_deadline(), start + 20s);
}

TEST(Deadline, IsUnsetWhenNothingIsPending) {
  Harness h;

  h.go_online();

  EXPECT_FALSE(h.core().next_deadline().has_value());
}

}  // namespace
}  // namespace porch::test
