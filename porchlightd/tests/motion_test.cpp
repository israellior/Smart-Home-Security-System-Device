#include <gtest/gtest.h>

#include "support.h"

namespace porch::test {
namespace {

TEST(Motion, AlertsBeforeItRecords) {
  Harness h;
  h.go_online();

  const auto out = h.send(MotionDetected{});

  EXPECT_EQ(names(out), (std::vector<std::string>{"SendAlert", "StartRecording", "SetLed"}));

  const SendAlert* alert = find<SendAlert>(out);
  ASSERT_NE(alert, nullptr);
  EXPECT_EQ(alert->kind, Kind::Motion);
  EXPECT_EQ(alert->triggered_at, h.now());

  const StartRecording* recording = find<StartRecording>(out);
  ASSERT_NE(recording, nullptr);
  EXPECT_EQ(recording->seconds, h.policy().clip);
  // The clip is filed against the id the server was already told about.
  EXPECT_EQ(recording->event_id, alert->event_id);
}

TEST(Motion, IsIgnoredDuringTheCooldown) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  // Settled, so the cooldown is the only thing left that could hold the next
  // alert back.
  h.send(AlertResult{"evt-1", Kind::Motion, AlertOutcome::Delivered});
  h.send(failed_clip("evt-1"));

  const auto out = h.after(19s, MotionDetected{});

  EXPECT_EQ(count<SendAlert>(out), 0);
  EXPECT_EQ(count<StartRecording>(out), 0);
}

TEST(Motion, TriggersAgainOnceTheCooldownLapses) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  h.send(AlertResult{"evt-1", Kind::Motion, AlertOutcome::Delivered});
  h.send(failed_clip("evt-1"));

  const auto out = h.after(20s, MotionDetected{});

  ASSERT_NE(find<SendAlert>(out), nullptr);
  EXPECT_EQ(find<SendAlert>(out)->event_id, "evt-2");
  EXPECT_NE(find<StartRecording>(out), nullptr);
}

TEST(Motion, DuringALiveCallAlertsWithoutRecording) {
  Harness h;
  h.go_online();
  h.send(ViewerRequested{"viewer-1"});

  const auto out = h.send(MotionDetected{});

  ASSERT_NE(find<SendAlert>(out), nullptr);
  EXPECT_EQ(find<SendAlert>(out)->kind, Kind::Motion);
  // The camera is already busy carrying the call.
  EXPECT_EQ(count<StartRecording>(out), 0);
}

TEST(Motion, DuringARingRecordingNeitherAlertsNorDowngrades) {
  Harness h;
  h.go_online();
  h.send(ButtonPressed{});  // a ring recording, which sets no motion cooldown
  h.send(AlertResult{"evt-1", Kind::Ring, AlertOutcome::Delivered});

  const auto out = h.after(1s, MotionDetected{});

  EXPECT_EQ(count<SendAlert>(out), 0);
  EXPECT_EQ(count<StartRecording>(out), 0);
}

}  // namespace
}  // namespace porch::test
