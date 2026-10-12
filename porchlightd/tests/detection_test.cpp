// Person detection: the PIR wakes the camera, and a motion alert waits for the
// camera's verdict. Every test here is one of the ways that wait can end - a
// verdict, the deadline, the recording ending first, or something else
// happening while it runs - because each of them has to end in exactly one
// alert or a deliberate silence, and never in a lost one.

#include <gtest/gtest.h>

#include "support.h"

namespace porch::test {
namespace {

// Eight seconds to look and three more to wait, inside a ten-second clip.
Policy detecting_policy() {
  Policy policy = test_policy();
  policy.person_detection = true;
  policy.detect_window = 8s;
  policy.detect_grace = 3s;
  policy.no_person_cooldown = 5s;
  return policy;
}

DetectionResult verdict(Verdict v, EventId id = "evt-1") { return DetectionResult{id, v}; }

TEST(Detection, MotionRecordsAndLooksButHoldsTheAlert) {
  Harness h(detecting_policy());
  h.go_online();

  const auto out = h.send(MotionDetected{});

  EXPECT_EQ(count<SendAlert>(out), 0);
  const StartRecording* recording = find<StartRecording>(out);
  ASSERT_NE(recording, nullptr);
  EXPECT_EQ(recording->seconds, h.policy().clip);
  EXPECT_EQ(recording->detect_for, h.policy().detect_window);
}

TEST(Detection, APersonReleasesTheAlertStampedWithThePirsTime) {
  Harness h(detecting_policy());
  h.go_online();
  const TimePoint fired = h.now();
  h.send(MotionDetected{});

  const auto out = h.after(2s, verdict(Verdict::Person));

  const SendAlert* alert = find<SendAlert>(out);
  ASSERT_NE(alert, nullptr);
  EXPECT_EQ(alert->event_id, "evt-1");
  EXPECT_EQ(alert->kind, Kind::Motion);
  // When the person was at the door, not when the camera made up its mind.
  EXPECT_EQ(alert->triggered_at, fired);
  // A person is what the clip is for: it records to its full length.
  EXPECT_EQ(count<StopRecording>(out), 0);
}

TEST(Detection, APersonsClipIsUploaded) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});
  h.after(2s, verdict(Verdict::Person));

  const auto out = h.after(8s, good_clip("evt-1"));

  ASSERT_NE(find<UploadClip>(out), nullptr);
  EXPECT_EQ(find<UploadClip>(out)->kind, Kind::Motion);
}

TEST(Detection, NobodyMeansNoAlertAndTheRecordingStops) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});

  const auto out = h.after(8s, verdict(Verdict::NoPerson));

  EXPECT_EQ(count<SendAlert>(out), 0);
  EXPECT_EQ(count<StopRecording>(out), 1);
}

TEST(Detection, NobodysClipIsThrownAwayAndNeverAlerts) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});
  h.after(8s, verdict(Verdict::NoPerson));

  // Long enough to clear min_clip, so only the verdict can be what discards it.
  const auto out = h.after(1s, good_clip("evt-1", 8s));

  EXPECT_EQ(count<UploadClip>(out), 0);
  EXPECT_EQ(count<DiscardClip>(out), 1);
  EXPECT_EQ(count<SendAlert>(out), 0);
}

TEST(Detection, UnknownAlertsAsIfNoDetectorExisted) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});

  const auto out = h.after(3s, verdict(Verdict::Unknown));

  ASSERT_NE(find<SendAlert>(out), nullptr);
  EXPECT_EQ(find<SendAlert>(out)->kind, Kind::Motion);
  EXPECT_EQ(count<StopRecording>(out), 0);
}

TEST(Detection, TheCoreKeepsItsOwnDeadline) {
  Harness h(detecting_policy());
  h.go_online();
  const TimePoint fired = h.now();
  h.send(MotionDetected{});

  EXPECT_EQ(h.core().next_deadline(), fired + 11s);
  EXPECT_EQ(count<SendAlert>(h.after(10s)), 0);
}

TEST(Detection, ASilentDetectorCannotSwallowTheAlert) {
  Harness h(detecting_policy());
  h.go_online();
  const TimePoint fired = h.now();
  h.send(MotionDetected{});

  // A hung Python thread, a Pi too hot to infer: nothing ever arrives.
  const auto out = h.after(11s);

  const SendAlert* alert = find<SendAlert>(out);
  ASSERT_NE(alert, nullptr);
  EXPECT_EQ(alert->triggered_at, fired);
}

TEST(Detection, AVerdictAfterTheDeadlineChangesNothing) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});
  h.after(11s);  // the alert went out unjudged

  const auto out = h.after(1s, verdict(Verdict::NoPerson));

  // Too late to unsend, and too late to stop or discard on its say-so either.
  EXPECT_EQ(count<SendAlert>(out), 0);
  EXPECT_EQ(count<StopRecording>(out), 0);
}

TEST(Detection, ARecordingThatDiesFirstStillAlerts) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});

  // The camera was held by something else, say: no frames, no verdict.
  const auto out = h.after(1s, failed_clip("evt-1"));

  ASSERT_NE(find<SendAlert>(out), nullptr);
  EXPECT_EQ(find<SendAlert>(out)->event_id, "evt-1");
}

TEST(Detection, OneAlertOnlyWhenTheRecordingEndsAfterTheVerdict) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});
  h.after(2s, verdict(Verdict::Person));

  const auto out = h.after(8s, good_clip("evt-1"));

  EXPECT_EQ(count<SendAlert>(out), 0);
}

TEST(Detection, ANewMotionIsLookedForSoonAfterNobody) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});
  h.after(8s, verdict(Verdict::NoPerson));
  h.after(1s, good_clip("evt-1", 8s));

  // Nine seconds after the first trigger, deep inside its twenty-second
  // cooldown - but the camera said nobody, so only the short one applies.
  EXPECT_EQ(count<StartRecording>(h.after(3s, MotionDetected{})), 0);
  const auto out = h.after(2s, MotionDetected{});

  ASSERT_NE(find<StartRecording>(out), nullptr);
  EXPECT_EQ(find<StartRecording>(out)->event_id, "evt-2");
}

TEST(Detection, APressWhileLookingRingsAtOnceAndReplacesTheMotionAlert) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});

  const auto pressed = h.after(1s, ButtonPressed{});

  EXPECT_EQ(count<PlayChime>(pressed), 1);
  const SendAlert* ring = find<SendAlert>(pressed);
  ASSERT_NE(ring, nullptr);
  EXPECT_EQ(ring->kind, Kind::Ring);
  EXPECT_EQ(ring->event_id, "evt-1");

  // The detector finishes its look regardless. Nothing it says matters now.
  const auto later = h.after(1s, verdict(Verdict::Person));
  EXPECT_EQ(count<SendAlert>(later), 0);
  EXPECT_EQ(count<SendAlert>(h.after(20s)), 0);
}

TEST(Detection, APressAfterNobodyRescuesTheClipAsARing) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});
  h.after(8s, verdict(Verdict::NoPerson));

  // The recording is still flushing when somebody the camera missed presses.
  const auto pressed = h.send(ButtonPressed{});
  ASSERT_NE(find<SendAlert>(pressed), nullptr);
  EXPECT_EQ(find<SendAlert>(pressed)->kind, Kind::Ring);
  EXPECT_EQ(find<SendAlert>(pressed)->event_id, "evt-1");
  // The camera is still busy with the old recording; it is the one kept.
  EXPECT_EQ(count<StartRecording>(pressed), 0);

  const auto out = h.after(1s, good_clip("evt-1", 8s));
  const UploadClip* clip = find<UploadClip>(out);
  ASSERT_NE(clip, nullptr);
  EXPECT_EQ(clip->kind, Kind::Ring);
  EXPECT_TRUE(clip->partial);  // stopped early by the verdict, not by its length
}

TEST(Detection, AViewerWhileLookingAlertsOnceTheRecorderLetsGo) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});

  const auto requested = h.after(2s, ViewerRequested{"viewer-1"});
  EXPECT_EQ(count<StopRecording>(requested), 1);
  EXPECT_EQ(count<SendAlert>(requested), 0);

  // Cut short before a verdict: nothing was judged, so it alerts as before.
  const auto out = h.after(1s, good_clip("evt-1", 3s));
  ASSERT_NE(find<SendAlert>(out), nullptr);
  EXPECT_EQ(find<SendAlert>(out)->kind, Kind::Motion);
  EXPECT_EQ(count<StartCall>(out), 1);
}

TEST(Detection, MotionDuringACallAlertsUnjudged) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(ViewerRequested{"viewer-1"});

  const auto out = h.send(MotionDetected{});

  // The camera is the call's, so there is nothing to look with.
  ASSERT_NE(find<SendAlert>(out), nullptr);
  EXPECT_EQ(count<StartRecording>(out), 0);
}

TEST(Detection, ARingRecordingDoesNotLook) {
  Harness h(detecting_policy());
  h.go_online();

  const auto out = h.send(ButtonPressed{});

  ASSERT_NE(find<StartRecording>(out), nullptr);
  EXPECT_EQ(find<StartRecording>(out)->detect_for, 0s);
  ASSERT_NE(find<SendAlert>(out), nullptr);  // and the ring never waits
}

TEST(Detection, AVerdictForAnotherRecordingIsIgnored) {
  Harness h(detecting_policy());
  h.go_online();
  h.send(MotionDetected{});

  const auto out = h.after(2s, verdict(Verdict::NoPerson, "evt-9"));

  EXPECT_EQ(count<StopRecording>(out), 0);
  // Still waiting on its own answer.
  ASSERT_NE(find<SendAlert>(h.after(1s, verdict(Verdict::Person))), nullptr);
}

TEST(Detection, OffMeansMotionAlertsAtOnce) {
  Harness h;  // test_policy leaves detection off
  h.go_online();

  const auto out = h.send(MotionDetected{});

  ASSERT_NE(find<SendAlert>(out), nullptr);
  ASSERT_NE(find<StartRecording>(out), nullptr);
  EXPECT_EQ(find<StartRecording>(out)->detect_for, 0s);
}

}  // namespace
}  // namespace porch::test
