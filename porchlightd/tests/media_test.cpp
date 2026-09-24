// What the daemon runs to bring up a call. build_media_command is exported for
// the same reason build_pipeline is: it is the one part of the live call that
// can be checked without a camera, a sound card, a network or a Pi - and one of
// the things asserted here is a security property rather than a preference.

#include "io/script_media.h"

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

// The value after a flag, or "" when the flag is absent or last.
std::string value_of(const std::vector<std::string>& args, const std::string& flag) {
  const auto it = std::find(args.begin(), args.end(), flag);
  if (it == args.end() || it + 1 == args.end()) {
    return "";
  }
  return *(it + 1);
}

ServerConfig server_config() {
  ServerConfig config;
  config.base_url = "https://porchlight.example";
  config.credential_path = "/etc/porchlight/credential";
  return config;
}

std::vector<std::string> command(const MediaConfig& media) {
  return build_media_command(server_config(), media, "porch-1");
}

// The whole reason the interpreter is configurable. The LiveKit SDK is a pip
// package and python3-gi is an apt one, so the call runs under a venv built
// with --system-site-packages; exec'ing the script itself would take the
// shebang, find the system python, and fail on `from livekit import rtc`.
TEST(MediaCommand, RunsTheVenvInterpreterAndNotTheScriptItself) {
  MediaConfig media;
  media.python = "/opt/porchlight/venv/bin/python3";
  media.script = "/usr/local/lib/porchlight/webrtc-video.py";
  const auto args = command(media);
  ASSERT_GE(args.size(), 2u);
  EXPECT_EQ(args[0], "/opt/porchlight/venv/bin/python3");
  EXPECT_EQ(args[1], "/usr/local/lib/porchlight/webrtc-video.py");
}

// Arguments are world-readable in /proc, so a credential on a command line is a
// credential handed to every account on the machine. It travels as a path and
// the child reads the file itself.
TEST(MediaCommand, PassesTheCredentialAsAPathAndNeverAsAValue) {
  ServerConfig server = server_config();
  server.credential_path = "/etc/porchlight/credential";
  const auto args = build_media_command(server, MediaConfig{}, "porch-1");
  EXPECT_EQ(value_of(args, "--credential-file"), "/etc/porchlight/credential");
  // Nothing that looks like the secret itself, under any flag.
  EXPECT_FALSE(contains(args, "pl_"));
  EXPECT_FALSE(has(args, "--credential"));
}

// There is no SDP on any socket of ours any more: what the call needs from the
// app server is a token, and what it needs to ask for one is the base URL and
// the device id. No peer, no room, no LiveKit URL - that comes back with the
// token and is never configured here.
TEST(MediaCommand, AsksForTokensWithTheDeviceIdentity) {
  const auto args = command(MediaConfig{});
  EXPECT_EQ(value_of(args, "--url"), "https://porchlight.example");
  EXPECT_EQ(value_of(args, "--device-id"), "porch-1");
  EXPECT_FALSE(contains(args, "livekit.cloud"));
  EXPECT_FALSE(has(args, "--peer"));
  EXPECT_FALSE(has(args, "--room"));
}

// The script takes one WIDTHxHEIGHT, not two numbers.
TEST(MediaCommand, FormatsTheSizeTheWayTheScriptParsesIt) {
  MediaConfig media;
  media.width = 640;
  media.height = 360;
  EXPECT_EQ(value_of(command(media), "--size"), "640x360");
}

// One card, a few centimetres apart. Off is a deliberate choice and has to look
// like one on the command line.
TEST(MediaCommand, EchoCancellationIsOnUnlessItIsTurnedOff) {
  EXPECT_EQ(value_of(command(MediaConfig{}), "--aec"), "on");
  MediaConfig media;
  media.echo_cancel = false;
  EXPECT_EQ(value_of(command(media), "--aec"), "off");
}

// A speaker device and --no-talkback are contradictory, so only one is ever
// sent. The script still watches the room either way - that is how it knows
// when the call is over.
TEST(MediaCommand, TalkbackIsEitherADeviceOrItsAbsence) {
  MediaConfig media;
  media.speaker_device = "hw:CARD=Headphones";
  const auto on = command(media);
  EXPECT_EQ(value_of(on, "--speaker-device"), "hw:CARD=Headphones");
  EXPECT_FALSE(has(on, "--no-talkback"));

  media.talkback = false;
  const auto off = command(media);
  EXPECT_TRUE(has(off, "--no-talkback"));
  EXPECT_FALSE(has(off, "--speaker-device"));
}

// --focus on videotestsrc and --mic-device with no microphone describe nothing,
// and an argument that describes nothing is one to be read wrongly later.
TEST(MediaCommand, SendsOnlyTheOptionsThatMeanSomethingForTheChosenSources) {
  MediaConfig media;
  media.video_source = "test";
  media.audio_source = "none";
  const auto args = command(media);
  EXPECT_FALSE(has(args, "--focus"));
  EXPECT_FALSE(has(args, "--mic-device"));
  EXPECT_EQ(value_of(args, "--video"), "test");
  EXPECT_EQ(value_of(args, "--audio"), "none");
}

TEST(MediaCommand, SendsFocusAndMicWhenTheyApply) {
  MediaConfig media;  // defaults: the camera and the real microphone
  media.focus = "1.5";
  media.mic_device = "hw:CARD=wm8960soundcard";
  const auto args = command(media);
  EXPECT_EQ(value_of(args, "--focus"), "1.5");
  EXPECT_EQ(value_of(args, "--mic-device"), "hw:CARD=wm8960soundcard");
}

// The daemon starts a call, not a diagnostic. Either of these would exit
// without ever publishing, and the viewer would wait for a picture that was
// never coming while the journal said everything had gone well.
TEST(MediaCommand, NeverRunsTheDiagnosticModes) {
  const auto args = command(MediaConfig{});
  EXPECT_FALSE(has(args, "--check"));
  EXPECT_FALSE(has(args, "--dry-run"));
}

// Both are seconds and both are how the call ends by itself, which is the only
// way it normally ends: nothing on this side can see who is in the room.
TEST(MediaCommand, CarriesTheTwoTimeoutsThatEndTheCall) {
  MediaConfig media;
  media.idle_timeout = std::chrono::seconds{45};
  media.linger = std::chrono::seconds{2};
  const auto args = command(media);
  EXPECT_EQ(value_of(args, "--idle-timeout"), "45");
  EXPECT_EQ(value_of(args, "--linger"), "2");
}

}  // namespace
}  // namespace porch
