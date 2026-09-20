#include <gtest/gtest.h>

#include "support.h"

namespace porch::test {
namespace {

// Motion, then a failed recording so the next motion is not suppressed by the
// one still in progress. Offline throughout, so the alerts pile up.
void trigger_offline_motion(Harness& h, const EventId& id) {
  h.send(MotionDetected{});
  h.send(failed_clip(id));
}

TEST(Alerts, AreHeldWhileOfflineAndSentInOrder) {
  Harness h;
  trigger_offline_motion(h, "evt-1");
  h.after(20s);
  trigger_offline_motion(h, "evt-2");

  const auto online = h.go_online();

  ASSERT_NE(find<SendAlert>(online), nullptr);
  EXPECT_EQ(find<SendAlert>(online)->event_id, "evt-1");

  const auto next = h.send(AlertResult{"evt-1", Kind::Motion, AlertOutcome::Delivered});
  ASSERT_NE(find<SendAlert>(next), nullptr);
  EXPECT_EQ(find<SendAlert>(next)->event_id, "evt-2");
}

TEST(Alerts, KeepTheTimeTheSensorFiredRatherThanTheTimeTheyAreSent) {
  Harness h;
  const TimePoint fired_at = h.now();
  trigger_offline_motion(h, "evt-1");

  const auto online = h.after(45s, ServerOnline{});

  ASSERT_NE(find<SendAlert>(online), nullptr);
  EXPECT_EQ(find<SendAlert>(online)->triggered_at, fired_at);
}

TEST(Alerts, DropTheOldestAtCapacity) {
  Harness h;  // capacity is three
  trigger_offline_motion(h, "evt-1");
  h.after(20s);
  trigger_offline_motion(h, "evt-2");
  h.after(20s);
  trigger_offline_motion(h, "evt-3");
  h.after(20s);
  trigger_offline_motion(h, "evt-4");

  const auto online = h.go_online();

  // The oldest went, because after an outage the recent events are the useful
  // ones.
  ASSERT_NE(find<SendAlert>(online), nullptr);
  EXPECT_EQ(find<SendAlert>(online)->event_id, "evt-2");
}

TEST(Alerts, TooOldToBeWorthSendingAreDropped) {
  Harness h;
  trigger_offline_motion(h, "evt-1");

  const auto online = h.after(61s, ServerOnline{});

  EXPECT_EQ(count<SendAlert>(online), 0);
}

TEST(Alerts, ThatFailAreRetriedAfterTheBackoff) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});

  const auto failed = h.send(AlertResult{"evt-1", Kind::Motion, AlertOutcome::Failed});
  EXPECT_EQ(count<SendAlert>(failed), 0);

  const auto too_soon = h.after(1s);
  EXPECT_EQ(count<SendAlert>(too_soon), 0);

  const auto retried = h.after(1s);
  ASSERT_NE(find<SendAlert>(retried), nullptr);
  EXPECT_EQ(find<SendAlert>(retried)->event_id, "evt-1");
}

TEST(Alerts, TheBackoffGrows) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});

  h.send(AlertResult{"evt-1", Kind::Motion, AlertOutcome::Failed});
  const auto second_try = h.after(2s);
  ASSERT_EQ(count<SendAlert>(second_try), 1);

  h.send(AlertResult{"evt-1", Kind::Motion, AlertOutcome::Failed});
  const auto still_waiting = h.after(2s);
  EXPECT_EQ(count<SendAlert>(still_waiting), 0);

  const auto third_try = h.after(2s);
  EXPECT_EQ(count<SendAlert>(third_try), 1);
}

TEST(Alerts, ThatTheServerRejectsAreNotRetried) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});

  h.send(AlertResult{"evt-1", Kind::Motion, AlertOutcome::Rejected});
  const auto later = h.after(30s);

  // A rejection is the server's answer, and asking again gets the same one.
  EXPECT_EQ(count<SendAlert>(later), 0);
}

TEST(Alerts, InFlightWhenTheLinkDropsAreSentAgain) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});  // evt-1 is now in flight

  h.send(ServerOffline{});
  const auto back = h.after(3s, ServerOnline{});

  ASSERT_NE(find<SendAlert>(back), nullptr);
  EXPECT_EQ(find<SendAlert>(back)->event_id, "evt-1");
}

}  // namespace
}  // namespace porch::test
