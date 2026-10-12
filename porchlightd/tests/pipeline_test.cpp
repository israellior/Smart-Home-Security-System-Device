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
// With both present every clip came out as ten video frames spread over 204
// hours. Pinning the clock is the only thing the Pi recorded correctly, and
// gst-launch cannot do it - hence record-clip.py and this flag.
TEST(Pipeline, CameraWithAlsaPinsTheSystemClock) {
  EXPECT_TRUE(has(build_pipeline(camera_config(), "/tmp/x.mp4"), "--system-clock"));
}

// This was the previous fix, and a test asserted it for four days while every
// clip was broken. check-clip.py (now in the git history) recorded it on the
// Pi: no better than nothing.
// Kept out so nobody reads its presence as the thing that makes clips work.
TEST(Pipeline, CameraDoesNotRelyOnTheCardDecliningTheClock) {
  EXPECT_FALSE(has(build_pipeline(camera_config(), "/tmp/x.mp4"), "provide-clock=false"));
}

// And only with the camera. The videotestsrc path was verified against a real
// card with the card providing the clock, and videotestsrc follows whichever
// clock the pipeline picked - so there is nothing to fix there to disturb.
TEST(Pipeline, TestSourceWithAlsaLeavesTheClockAlone) {
  RecorderConfig config = camera_config();
  config.video_source = "test";
  EXPECT_FALSE(has(build_pipeline(config, "/tmp/x.mp4"), "--system-clock"));
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

// record-clip.py is what turns SIGINT into EOS, and EOS is what writes the
// moov atom. Everything after -- is handed to parse_launchv untouched, so a
// flag of its own landing on the wrong side would be a parse failure.
TEST(Pipeline, RunsTheRecorderScriptAndNamesTheFile) {
  RecorderConfig config = camera_config();
  config.script = "/opt/x/record-clip.py";
  const auto args = build_pipeline(config, "/tmp/clip.mp4");
  ASSERT_FALSE(args.empty());
  EXPECT_EQ(args.front(), "/opt/x/record-clip.py");
  const auto separator = std::find(args.begin(), args.end(), std::string{"--"});
  ASSERT_NE(separator, args.end());
  EXPECT_LT(std::find(args.begin(), args.end(), std::string{"--system-clock"}), separator);
  EXPECT_EQ(*(separator + 1), "mp4mux");
  EXPECT_TRUE(contains(args, "location=\"/tmp/clip.mp4\""));
}

// Each branch needs its own queue or the capture thread does the encoding too,
// and the sound card overruns while it is busy elsewhere.
TEST(Pipeline, BothBranchesKeepTheirQueues) {
  const auto args = build_pipeline(camera_config(), "/tmp/x.mp4");
  EXPECT_GE(std::count(args.begin(), args.end(), std::string{"queue"}), 3);
}

// Person detection rides on the recording rather than opening the camera,
// because only one process can hold it. These are its rules as strings.
using namespace std::chrono_literals;

TEST(Pipeline, WithoutDetectionThereIsNoTeeAndNoDetector) {
  const auto args = build_pipeline(camera_config(), "/tmp/x.mp4");
  EXPECT_FALSE(has(args, "tee"));
  EXPECT_FALSE(contains(args, "appsink"));
  EXPECT_FALSE(contains(args, "--detect"));
}

TEST(Pipeline, DetectionSplitsTheCameraIntoAnAppsink) {
  const auto args = build_pipeline(camera_config(), "/tmp/x.mp4", 8s);
  EXPECT_TRUE(has(args, "tee"));
  EXPECT_TRUE(has(args, "name=t"));
  EXPECT_TRUE(has(args, "t."));
  EXPECT_TRUE(has(args, "appsink"));
  // The name record-clip.py looks it up by.
  EXPECT_TRUE(has(args, "name=detect"));
}

// The one rule the branch exists to keep: a slow detector costs judged frames,
// never recorded ones. A queue that blocks would back up into the tee and
// stall the encoder with it.
TEST(Pipeline, TheDetectorBranchCanNeverStallTheClip) {
  const auto args = build_pipeline(camera_config(), "/tmp/x.mp4", 8s);
  EXPECT_TRUE(has(args, "leaky=downstream"));
  EXPECT_TRUE(has(args, "drop=true"));
  EXPECT_TRUE(has(args, "max-buffers=1"));
  EXPECT_TRUE(has(args, "sync=false"));
}

TEST(Pipeline, TheDetectorGetsSquareRgbAtItsOwnRate) {
  RecorderConfig config = camera_config();
  config.detect.input_size = 320;
  config.detect.fps = 4;
  const auto args = build_pipeline(config, "/tmp/x.mp4", 8s);
  EXPECT_TRUE(has(args, "video/x-raw,format=RGB,width=320,height=320"));
  EXPECT_TRUE(has(args, "max-rate=4"));
}

// The inference runtime is a pip package, so the venv's interpreter runs the
// script; its switches go before the --, where parse_launchv will not see them.
TEST(Pipeline, DetectionRunsUnderTheVenvWithItsSwitchesBeforeTheSeparator) {
  RecorderConfig config = camera_config();
  config.script = "/opt/x/record-clip.py";
  config.detect.python = "/opt/venv/bin/python3";
  config.detect.model = "/opt/x/person.tflite";
  const auto args = build_pipeline(config, "/tmp/x.mp4", 6s);

  ASSERT_GE(args.size(), 2u);
  EXPECT_EQ(args[0], "/opt/venv/bin/python3");
  EXPECT_EQ(args[1], "/opt/x/record-clip.py");

  const auto separator = std::find(args.begin(), args.end(), std::string{"--"});
  ASSERT_NE(separator, args.end());
  const auto model = std::find(args.begin(), args.end(), std::string{"--detect-model"});
  ASSERT_LT(model, separator);
  EXPECT_EQ(*(model + 1), "/opt/x/person.tflite");
  const auto window = std::find(args.begin(), args.end(), std::string{"--detect-window"});
  ASSERT_LT(window, separator);
  // The core's window, carried on StartRecording, not a second copy in config.
  EXPECT_EQ(*(window + 1), "6");
}

// The muxer's branches are unchanged by the tee: still a queue each side.
TEST(Pipeline, DetectionKeepsTheRecordingsOwnQueues) {
  const auto args = build_pipeline(camera_config(), "/tmp/x.mp4", 8s);
  EXPECT_GE(std::count(args.begin(), args.end(), std::string{"queue"}), 4);
}

}  // namespace
}  // namespace porch
