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

TEST(Led, ShowsFaultWhenTheLinkHasGivenUpRatherThanMerelyDropped) {
  // The whole reason ServerOffline carries a flag. A rejected credential and a
  // flat router look identical from every other angle, and only one of them is
  // fixed by waiting - a doorbell with one LED has nowhere else to say so.
  Harness h;
  h.go_online();

  const auto out = h.send(ServerOffline{true});

  ASSERT_NE(find<SetLed>(out), nullptr);
  EXPECT_EQ(find<SetLed>(out)->pattern, LedPattern::Fault);
}

TEST(Led, KeepsShowingFaultWhenAPlainOutageIsReportedAfterIt) {
  // The link reports "down" on every failed attempt after it has given up, and
  // a later ordinary report must not talk the light back down into "the
  // network will be along shortly".
  Harness h;
  h.go_online();
  h.send(ServerOffline{true});

  const auto out = h.send(ServerOffline{false});

  EXPECT_EQ(led_after(out, LedPattern::Fault), LedPattern::Fault);
}

TEST(Led, LeavesFaultOnlyWhenSomethingActuallyConnects) {
  Harness h;
  h.send(ServerOffline{true});

  const auto out = h.go_online();

  EXPECT_EQ(led_after(out, LedPattern::Fault), LedPattern::Idle);
}

TEST(Led, PrefersARecordingToAFault) {
  // A fault is persistent and a recording is fifteen seconds, so the order is
  // the same one the rest of the table uses: show the thing that is happening
  // now, and go back to the standing complaint afterwards.
  Harness h;
  h.send(ServerOffline{true});

  const auto out = h.send(MotionDetected{});

  EXPECT_EQ(led_after(out, LedPattern::Fault), LedPattern::Recording);
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
