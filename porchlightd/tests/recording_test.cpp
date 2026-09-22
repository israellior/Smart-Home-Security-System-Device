#include <gtest/gtest.h>

#include "support.h"

namespace porch::test {
namespace {

TEST(Recording, AGoodClipIsUploaded) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});

  const auto out = h.after(10s, good_clip("evt-1"));

  ASSERT_NE(find<UploadClip>(out), nullptr);
  EXPECT_EQ(find<UploadClip>(out)->path, "/spool/clip.mp4");
  EXPECT_EQ(count<DiscardClip>(out), 0);
}

TEST(Recording, AnUploadCarriesEverythingTheConfirmNeeds) {
  Harness h;
  h.go_online();
  const TimePoint fired = h.now();
  h.send(MotionDetected{});

  const auto out = h.after(10s, good_clip("evt-1", 9500ms));

  const UploadClip* clip = find<UploadClip>(out);
  ASSERT_NE(clip, nullptr);
  EXPECT_EQ(clip->kind, Kind::Motion);
  // When the sensor fired, not when the recorder finished ten seconds later.
  EXPECT_EQ(clip->triggered_at, fired);
  EXPECT_EQ(clip->duration, 9500ms);
  EXPECT_FALSE(clip->partial);
}

TEST(Recording, AnUpgradedEventUploadsAsARingFromWhenTheMotionFired) {
  Harness h;
  h.go_online();
  const TimePoint fired = h.now();
  h.send(MotionDetected{});
  h.after(2s, ButtonPressed{});

  const auto out = h.after(8s, good_clip("evt-1"));

  const UploadClip* clip = find<UploadClip>(out);
  ASSERT_NE(clip, nullptr);
  EXPECT_EQ(clip->kind, Kind::Ring);
  // The press raises the kind but not the time: the clip still begins where
  // the motion did, and its first frame is from then.
  EXPECT_EQ(clip->triggered_at, fired);
}

TEST(Recording, AClipAViewerCutShortIsMarkedPartial) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});

  const auto stopped = h.after(3s, ViewerRequested{"viewer-1"});
  ASSERT_EQ(count<StopRecording>(stopped), 1);

  // The upload waits for the call to end, so the flag has to outlive both the
  // recording and the viewer that ended it.
  h.after(1s, good_clip("evt-1", 4s));
  const auto out = h.send(CallEnded{"viewer-1", "closed the tab"});

  const UploadClip* clip = find<UploadClip>(out);
  ASSERT_NE(clip, nullptr);
  EXPECT_TRUE(clip->partial);
  EXPECT_EQ(clip->duration, 4s);
}

TEST(Recording, AClipTheCoreCannotDescribeIsDiscarded) {
  Harness h;
  h.go_online();

  // Nothing here started evt-9, so there is no kind and no trigger time to
  // confirm it with, and a wrong row is worse than no row.
  const auto out = h.send(good_clip("evt-9"));

  EXPECT_EQ(count<UploadClip>(out), 0);
  ASSERT_NE(find<DiscardClip>(out), nullptr);
  EXPECT_EQ(find<DiscardClip>(out)->event_id, "evt-9");
}

TEST(Recording, AClipShorterThanTheMinimumIsDiscarded) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});

  const auto out = h.after(1s, good_clip("evt-1", 1s));

  EXPECT_EQ(count<UploadClip>(out), 0);
  ASSERT_NE(find<DiscardClip>(out), nullptr);
  EXPECT_EQ(find<DiscardClip>(out)->event_id, "evt-1");
}

TEST(Recording, AFailedRecordingIsDiscarded) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});

  RecordingFinished died = failed_clip("evt-1");
  died.path = "/spool/clip.mp4";  // a killed muxer still leaves a file behind
  const auto out = h.after(4s, died);

  EXPECT_EQ(count<UploadClip>(out), 0);
  EXPECT_EQ(count<DiscardClip>(out), 1);
}

TEST(Recording, AnEmptyFileIsDiscardedEvenWhenTheRecorderClaimsSuccess) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});

  RecordingFinished empty = good_clip("evt-1");
  empty.bytes = 0;
  const auto out = h.after(10s, empty);

  EXPECT_EQ(count<UploadClip>(out), 0);
  EXPECT_EQ(count<DiscardClip>(out), 1);
}

TEST(Recording, TheOldestClipGoesWhenTheSpoolIsFull) {
  Harness h;  // offline on purpose: nothing leaves, so the clips pile up

  const auto record = [&h](const char* id) {
    h.after(25s, MotionDetected{});  // past the cooldown, so each one triggers
    return h.after(10s, good_clip(id, 10s, std::string("/spool/") + id + ".mp4"));
  };

  record("evt-1");
  record("evt-2");
  record("evt-3");  // three megabytes, which is exactly the cap
  const auto fourth = record("evt-4");

  ASSERT_NE(find<DiscardClip>(fourth), nullptr);
  EXPECT_EQ(find<DiscardClip>(fourth)->event_id, "evt-1");
  EXPECT_EQ(find<DiscardClip>(fourth)->path, "/spool/evt-1.mp4");
  EXPECT_EQ(count<DiscardClip>(fourth), 1);  // only as many as it takes to fit

  // And what survived is still in order behind it.
  const auto online = h.go_online();
  ASSERT_NE(find<UploadClip>(online), nullptr);
  EXPECT_EQ(find<UploadClip>(online)->event_id, "evt-2");
}

TEST(Recording, TheClipBeingUploadedIsNeverTheOneDropped) {
  Harness h;
  h.go_online();

  const auto record = [&h](const char* id) {
    h.after(25s, MotionDetected{});
    return h.after(10s, good_clip(id, 10s, std::string("/spool/") + id + ".mp4"));
  };

  // evt-1 goes out at once and then says nothing, so it stays in flight and
  // at the front of the queue while the others queue behind it.
  const auto first = record("evt-1");
  ASSERT_EQ(count<UploadClip>(first), 1);

  record("evt-2");
  record("evt-3");
  const auto fourth = record("evt-4");

  ASSERT_NE(find<DiscardClip>(fourth), nullptr);
  EXPECT_EQ(find<DiscardClip>(fourth)->event_id, "evt-2");
  EXPECT_EQ(count<UploadClip>(fourth), 0);  // still the one that never answered
}

TEST(Recording, AnUploadWaitsForTheServer) {
  Harness h;  // offline
  h.send(MotionDetected{});

  const auto finished = h.after(10s, good_clip("evt-1"));
  EXPECT_EQ(count<UploadClip>(finished), 0);

  const auto online = h.go_online();
  ASSERT_NE(find<UploadClip>(online), nullptr);
  EXPECT_EQ(find<UploadClip>(online)->event_id, "evt-1");
}

TEST(Recording, AFailedUploadIsRetriedAfterTheBackoff) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  h.after(10s, good_clip("evt-1"));

  const auto failed = h.send(UploadFinished{"evt-1", false});
  EXPECT_EQ(count<UploadClip>(failed), 0);

  const auto too_soon = h.after(1s);
  EXPECT_EQ(count<UploadClip>(too_soon), 0);

  const auto retried = h.after(1s);
  EXPECT_EQ(count<UploadClip>(retried), 1);
}

TEST(Recording, ARetryWaitsForAViewerToLeave) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  h.after(10s, good_clip("evt-1"));
  h.send(UploadFinished{"evt-1", false});  // the confirm failed; a retry is due

  h.send(ViewerRequested{"viewer-1"});
  const auto during = h.after(5s);  // long past the backoff
  EXPECT_EQ(count<UploadClip>(during), 0);

  const auto left = h.send(CallEnded{"viewer-1", "closed the tab"});
  ASSERT_NE(find<UploadClip>(left), nullptr);
  EXPECT_EQ(find<UploadClip>(left)->event_id, "evt-1");
}

TEST(Recording, AnUploadAlreadyInFlightIsNotCancelledByACall) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  const auto started = h.after(10s, good_clip("evt-1"));
  ASSERT_EQ(count<UploadClip>(started), 1);

  // There is no way to unsend it, so a viewer arriving does not try.
  const auto viewer = h.send(ViewerRequested{"viewer-1"});
  EXPECT_EQ(count<StopRecording>(viewer), 0);

  // And its result is still accepted while the call is live.
  h.send(UploadFinished{"evt-1", true});
  const auto later = h.after(30s);
  EXPECT_EQ(count<UploadClip>(later), 0);
}

TEST(Recording, AFailedConfirmKeepsTheClipForAnotherAttempt) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  h.after(10s, good_clip("evt-1"));

  // ok=false means the confirm failed, whether or not the bytes were sent.
  // The whole sequence starts again, so the same clip comes back.
  h.send(UploadFinished{"evt-1", false});
  const auto retried = h.after(2s);

  ASSERT_NE(find<UploadClip>(retried), nullptr);
  EXPECT_EQ(find<UploadClip>(retried)->event_id, "evt-1");
  EXPECT_EQ(find<UploadClip>(retried)->path, "/spool/clip.mp4");
}

TEST(Recording, ASucceededUploadIsNotRetried) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  h.after(10s, good_clip("evt-1"));

  h.send(UploadFinished{"evt-1", true});
  const auto later = h.after(30s);

  EXPECT_EQ(count<UploadClip>(later), 0);
}

}  // namespace
}  // namespace porch::test
