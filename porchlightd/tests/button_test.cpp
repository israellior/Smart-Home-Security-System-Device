#include <gtest/gtest.h>

#include "support.h"

namespace porch::test {
namespace {

TEST(Button, ChimesThenAlertsThenRecords) {
  Harness h;
  h.go_online();

  const auto out = h.send(ButtonPressed{});

  EXPECT_EQ(names(out),
            (std::vector<std::string>{"PlayChime", "SendAlert", "StartRecording", "SetLed"}));
  ASSERT_NE(find<SendAlert>(out), nullptr);
  EXPECT_EQ(find<SendAlert>(out)->kind, Kind::Ring);
}

TEST(Button, ChimesWithTheServerUnreachable) {
  Harness h;  // never goes online

  const auto out = h.send(ButtonPressed{});

  EXPECT_EQ(count<PlayChime>(out), 1);
  // The alert is queued, not sent, and that does not hold up the chime.
  EXPECT_EQ(count<SendAlert>(out), 0);
}

TEST(Button, PressedAgainInsideTheWindowChimesButDoesNotAlert) {
  Harness h;
  h.go_online();
  h.send(ButtonPressed{});
  // Settled, so the window is the only thing suppressing a second alert.
  h.send(AlertResult{"evt-1", Kind::Ring, AlertOutcome::Delivered});

  const auto out = h.after(4s, ButtonPressed{});

  EXPECT_EQ(count<PlayChime>(out), 1);
  EXPECT_EQ(count<SendAlert>(out), 0);
  EXPECT_EQ(count<StartRecording>(out), 0);
}

TEST(Button, PressedAfterTheWindowAlertsAgain) {
  Harness h;
  h.go_online();
  h.send(ButtonPressed{});
  h.send(AlertResult{"evt-1", Kind::Ring, AlertOutcome::Delivered});
  // The first clip is done; a press during one would upgrade it instead.
  h.send(failed_clip("evt-1"));

  const auto out = h.after(5s, ButtonPressed{});

  EXPECT_EQ(count<PlayChime>(out), 1);
  ASSERT_NE(find<SendAlert>(out), nullptr);
  EXPECT_EQ(find<SendAlert>(out)->event_id, "evt-2");
}

TEST(Button, DuringAMotionRecordingUpgradesTheSameEvent) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  h.send(AlertResult{"evt-1", Kind::Motion, AlertOutcome::Delivered});

  const auto out = h.after(2s, ButtonPressed{});

  EXPECT_EQ(count<PlayChime>(out), 1);
  const SendAlert* alert = find<SendAlert>(out);
  ASSERT_NE(alert, nullptr);
  EXPECT_EQ(alert->event_id, "evt-1");
  EXPECT_EQ(alert->kind, Kind::Ring);
  // One event, one clip: no new id was minted and no second recording began.
  EXPECT_EQ(h.ids().issued(), 1);
  EXPECT_EQ(count<StartRecording>(out), 0);
  EXPECT_EQ(count<StopRecording>(out), 0);
}

TEST(Button, UpgradesOnlyOnce) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  h.send(AlertResult{"evt-1", Kind::Motion, AlertOutcome::Delivered});
  h.after(2s, ButtonPressed{});
  h.send(AlertResult{"evt-1", Kind::Ring, AlertOutcome::Delivered});

  const auto out = h.after(6s, ButtonPressed{});  // past the de-dup window

  EXPECT_EQ(count<PlayChime>(out), 1);
  // Already a ring, so there is nothing left to upgrade and no second alert.
  EXPECT_EQ(count<SendAlert>(out), 0);
  EXPECT_EQ(count<StartRecording>(out), 0);
}

TEST(Button, DuringALiveCallChimesAndAlertsWithoutRecording) {
  Harness h;
  h.go_online();
  h.send(ViewerRequested{"viewer-1"});

  const auto out = h.send(ButtonPressed{});

  EXPECT_EQ(count<PlayChime>(out), 1);
  ASSERT_NE(find<SendAlert>(out), nullptr);
  EXPECT_EQ(find<SendAlert>(out)->kind, Kind::Ring);
  EXPECT_EQ(count<StartRecording>(out), 0);
}

}  // namespace
}  // namespace porch::test
