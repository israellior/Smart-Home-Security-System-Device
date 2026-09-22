// What the recorder asks GStreamer for. build_pipeline is exported precisely
// because this is the one part of recording that can be checked without a
// camera, a sound card or a Pi - every assertion here is a lesson that cost a
// session on real hardware and would otherwise survive only as a comment.

#include "io/gst_recorder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace porch {
namespace {

bool has(const std::vector<std::string>& args, const std::string& want) {
  return std::find(args.begin(), args.end(), want) != args.end();
}

bool contains(const std::vector<std::string>& args, const std::string& want) {
  return std::any_of(args.begin(), args.end(),
                     [&](const std::string& arg) { return arg.find(want) != std::string::npos; });
}

// The camera pointed at the real sound card: the combination that broke.
RecorderConfig camera_config() {
  RecorderConfig config;
  config.video_source = "libcamera";
  config.audio_source = "alsa";
  config.width = 640;
  config.height = 360;
  return config;
}

// alsasrc offers the sound card as the pipeline clock; libcamerasrc offers none
// and timestamps from the system monotonic clock whatever the pipeline chose.
// With both present the video branch stalls within a second while audio runs on
// perfectly. webrtc-video.py fixes it with pipeline.use_clock(); there is no
// such call from a gst-launch command line, so the card has to decline instead.
TEST(Pipeline, CameraWithAlsaMakesTheSoundCardGiveUpTheClock) {
  EXPECT_TRUE(has(build_pipeline(camera_config(), "/tmp/x.mp4"), "provide-clock=false"));
}

// And only then. The videotestsrc path was verified against a real card with
// the card providing the clock, and videotestsrc follows whichever clock the
// pipeline picked - so there is nothing to fix there and nothing to disturb.
TEST(Pipeline, TestSourceWithAlsaLeavesTheClockAlone) {
  RecorderConfig config = camera_config();
  config.video_source = "test";
  EXPECT_FALSE(has(build_pipeline(config, "/tmp/x.mp4"), "provide-clock=false"));
}

// Left alone libcamera picks the imx708's binned 1536x864 mode, which reads
// only the centre of the array. On the wide lens that is a third of the frame
// width gone, and nothing downstream can get it back.
TEST(Pipeline, CameraAsksForTheFullSensorArray) {
  EXPECT_TRUE(contains(build_pipeline(camera_config(), "/tmp/x.mp4"), "width=2304,height=1296"));
}

// Without this it negotiates NV21 and videoconvert de-interleaves every frame.
TEST(Pipeline, CameraPinsI420SoVideoconvertPassesThrough) {
  EXPECT_TRUE(contains(build_pipeline(camera_config(), "/tmp/x.mp4"),
                       "video/x-raw,format=I420,width=640,height=360"));
}

// af-mode defaults to manual at lens-position 0, which is infinity.
TEST(Pipeline, CameraSetsFocusBecauseItDefaultsToInfinity) {
  EXPECT_TRUE(has(build_pipeline(camera_config(), "/tmp/x.mp4"), "af-mode=continuous"));
}

// None of those belong on videotestsrc, and a stray one is a parse failure at
// record time - which is the worst moment to discover it.
TEST(Pipeline, TestSourceCarriesNoCameraProperties) {
  const RecorderConfig config;  // defaults: test video, test audio
  const auto args = build_pipeline(config, "/tmp/x.mp4");
  EXPECT_FALSE(has(args, "af-mode=continuous"));
  EXPECT_FALSE(contains(args, "sensor-config"));
  EXPECT_FALSE(contains(args, "format=I420"));
}

// -e is what turns SIGINT into EOS, and EOS is what writes the moov atom. A
// clip recorded without it is a file that will not play.
TEST(Pipeline, AlwaysFlushesTheMuxerAndNamesTheFile) {
  const auto args = build_pipeline(camera_config(), "/tmp/clip.mp4");
  ASSERT_FALSE(args.empty());
  EXPECT_EQ(args.front(), "gst-launch-1.0");
  EXPECT_TRUE(has(args, "-e"));
  EXPECT_TRUE(contains(args, "location=\"/tmp/clip.mp4\""));
}

// Each branch needs its own queue or the capture thread does the encoding too,
// and the sound card overruns while it is busy elsewhere.
TEST(Pipeline, BothBranchesKeepTheirQueues) {
  const auto args = build_pipeline(camera_config(), "/tmp/x.mp4");
  EXPECT_GE(std::count(args.begin(), args.end(), std::string{"queue"}), 3);
}

}  // namespace
}  // namespace porch
