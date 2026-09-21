#include <gtest/gtest.h>

#include "support.h"

namespace porch::test {
namespace {

TEST(Call, StartsImmediatelyWhenNothingElseIsRunning) {
  Harness h;
  h.go_online();

  const auto out = h.send(ViewerRequested{"viewer-1"});

  ASSERT_NE(find<StartCall>(out), nullptr);
  EXPECT_EQ(find<StartCall>(out)->peer, "viewer-1");
}

TEST(Call, WaitsForTheRecorderToFinishBeforeItStarts) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});

  const auto stopping = h.after(3s, ViewerRequested{"viewer-1"});

  EXPECT_EQ(count<StopRecording>(stopping), 1);
  // The recorder still holds the camera and the sound card until EOS lands.
  EXPECT_EQ(count<StartCall>(stopping), 0);

  const auto finished = h.after(1s, good_clip("evt-1", 4s));

  EXPECT_EQ(count<StartCall>(finished), 1);
  EXPECT_EQ(find<StartCall>(finished)->peer, "viewer-1");
}

TEST(Call, PartialClipIsKeptAndUploadedOnceTheCallEnds) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  h.after(3s, ViewerRequested{"viewer-1"});

  // The clip is worth keeping, but the upstream link belongs to the viewer.
  const auto finished = h.after(1s, good_clip("evt-1", 4s));
  EXPECT_EQ(count<StartCall>(finished), 1);
  EXPECT_EQ(count<UploadClip>(finished), 0);

  const auto left = h.send(CallEnded{"viewer-1", "closed the tab"});
  ASSERT_NE(find<UploadClip>(left), nullptr);
  EXPECT_EQ(find<UploadClip>(left)->event_id, "evt-1");
}

TEST(Call, ASecondViewerJoinsTheSameCall) {
  Harness h;
  h.go_online();
  h.send(ViewerRequested{"viewer-1"});

  const auto out = h.send(ViewerRequested{"viewer-2"});

  ASSERT_NE(find<StartCall>(out), nullptr);
  EXPECT_EQ(find<StartCall>(out)->peer, "viewer-2");
}

TEST(Call, StaysLiveUntilTheLastViewerLeaves) {
  Harness h;
  h.go_online();
  h.send(ViewerRequested{"viewer-1"});
  h.send(ViewerRequested{"viewer-2"});

  const auto one_left = h.send(CallEnded{"viewer-1", "closed the tab"});
  // Still live, so the light does not change.
  EXPECT_EQ(count<SetLed>(one_left), 0);

  const auto both_left = h.send(CallEnded{"viewer-2", "closed the tab"});
  ASSERT_NE(find<SetLed>(both_left), nullptr);
  EXPECT_EQ(find<SetLed>(both_left)->pattern, LedPattern::Idle);
}

TEST(Call, AViewerWhoGivesUpWhileStoppingDoesNotGetACall) {
  Harness h;
  h.go_online();
  h.send(MotionDetected{});
  h.after(3s, ViewerRequested{"viewer-1"});
  h.send(CallEnded{"viewer-1", "gave up"});

  const auto out = h.after(1s, good_clip("evt-1", 4s));

  EXPECT_EQ(count<StartCall>(out), 0);
}

}  // namespace
}  // namespace porch::test
