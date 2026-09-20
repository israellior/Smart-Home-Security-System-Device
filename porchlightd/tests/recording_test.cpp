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
