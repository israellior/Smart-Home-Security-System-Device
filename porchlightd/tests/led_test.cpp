#include <gtest/gtest.h>

#include "support.h"

namespace porch::test {
namespace {

LedPattern led_after(const std::vector<Action>& actions, LedPattern unchanged) {
  const SetLed* set = find<SetLed>(actions);
  return set ? set->pattern : unchanged;
}

TEST(Led, ShowsOfflineBeforeTheServerIsReached) {
  Harness h;

  const auto out = h.send(Tick{});

  ASSERT_NE(find<SetLed>(out), nullptr);
  EXPECT_EQ(find<SetLed>(out)->pattern, LedPattern::Offline);
}

TEST(Led, ShowsIdleOnceOnlineWithNothingHappening) {
  Harness h;

  const auto out = h.go_online();

  ASSERT_NE(find<SetLed>(out), nullptr);
  EXPECT_EQ(find<SetLed>(out)->pattern, LedPattern::Idle);
}

TEST(Led, PrefersRecordingToOffline) {
  Harness h;  // offline, so the light is showing Offline

  const auto out = h.send(MotionDetected{});

  EXPECT_EQ(led_after(out, LedPattern::Offline), LedPattern::Recording);
}

TEST(Led, PrefersALiveCallToARecording) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  h.after(2s, ViewerRequested{"viewer-1"});

  const auto out = h.after(1s, good_clip("evt-1", 3s));

  EXPECT_EQ(led_after(out, LedPattern::Recording), LedPattern::Live);
}

TEST(Led, PrefersARingToEverythingElse) {
  Harness h;
  h.go_online();
  h.send(ViewerRequested{"viewer-1"});

  const auto out = h.send(ButtonPressed{});

  EXPECT_EQ(led_after(out, LedPattern::Live), LedPattern::Ring);
}

TEST(Led, FallsBackToTheCallOnceTheRingLapses) {
  Harness h;
  h.go_online();
  h.send(ViewerRequested{"viewer-1"});
  h.send(ButtonPressed{});

  const auto out = h.after(3s);

  EXPECT_EQ(led_after(out, LedPattern::Ring), LedPattern::Live);
}

TEST(Led, GoesOutOnShutdown) {
  Harness h;
  h.go_online();

  const auto out = h.send(Shutdown{});

  ASSERT_NE(find<SetLed>(out), nullptr);
  EXPECT_EQ(find<SetLed>(out)->pattern, LedPattern::Off);
}

TEST(Led, IsNotSetAgainWhenNothingChanged) {
  Harness h;
  h.go_online();

  const auto out = h.after(1s);

  EXPECT_EQ(count<SetLed>(out), 0);
}

}  // namespace
}  // namespace porch::test
