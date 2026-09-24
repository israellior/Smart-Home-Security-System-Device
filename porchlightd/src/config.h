#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "core/policy.h"
#include "logging.h"

namespace porch {

// Thrown with the offending key in the message, so a typo in the JSON names
// itself instead of turning into a default.
struct ConfigError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Which implementation each interface gets. Only the fakes exist today.
struct Backends {
  std::string input{"stdin"};
  std::string led{"console"};
  std::string chime{"console"};
  std::string server{"log"};
  std::string uploader{"log"};
  std::string media{"stub"};
  std::string recorder{"fake"};
};

struct ServerConfig {
  std::string base_url{"http://127.0.0.1:4000"};
  // Written onto the card when the device is built. There is no enrolment
  // call, so a missing one means the device has to be re-minted.
  std::filesystem::path credential_path{"/etc/porchlight/credential"};
  std::filesystem::path bridge_path{"/usr/local/lib/porchlight/server-bridge.py"};
  // The other half of the same link, and deliberately a second process rather
  // than a second mode of the first: an upload is minutes of HTTPS to a
  // different host, and must not share a fate with the socket carrying alerts.
  std::filesystem::path uploader_path{"/usr/local/lib/porchlight/upload-clip.py"};
  // How long silence is allowed to last before it counts as a failure. The
  // server signals transient trouble by not answering at all.
  std::chrono::seconds ack_timeout{10};
};

// The live call, which is a separate program. Held apart from RecorderConfig
// even where the fields look the same: a recording has no latency requirement
// and a call is nothing but one, so the two will keep drifting apart.
struct MediaConfig {
  // The venv's interpreter. The LiveKit SDK is a pip package and python3-gi is
  // an apt one, so this must be a venv built with --system-site-packages or
  // the script cannot import both halves of what it needs.
  std::filesystem::path python{"/opt/porchlight/venv/bin/python3"};
  std::filesystem::path script{"/usr/local/lib/porchlight/webrtc-video.py"};

  std::string video_source{"camera"};  // camera | test
  // 16:9, because the imx708 is and the default source here is the camera.
  // Asking a 16:9 sensor for 4:3 makes the ISP crop the sides off, which on the
  // wide lens throws away the field of view it exists for.
  int width{640};
  int height{360};
  int fps{30};
  std::string codec{"h264"};  // h264 | vp8
  // A ceiling, not a target: LiveKit lowers it when the link cannot carry it.
  int video_bitrate_kbps{1500};
  std::string focus{"continuous"};  // continuous | default | metres

  std::string audio_source{"alsa"};  // alsa | test | none
  std::string mic_device{"hw:CARD=wm8960soundcard"};
  std::string speaker_device{"hw:CARD=wm8960soundcard"};
  // Whether viewers are played out of the speaker at all. Off makes the call
  // one-way, which is a way to prove the picture without the room howling.
  bool talkback{true};
  // Not optional in practice: the microphone and the speaker are one card and
  // a few centimetres apart.
  bool echo_cancel{true};

  // How long to wait for the first viewer before giving up on a request that
  // nobody turned up for, and how long to stay published after the last one
  // leaves so that a reloaded page is not a restart.
  std::chrono::seconds idle_timeout{30};
  std::chrono::seconds linger{3};
};

struct ChimeConfig {
  // plughw rather than hw, so ALSA converts whatever the file happens to be
  // into what the card accepts instead of refusing it at the wrong rate.
  std::string device{"plughw:CARD=wm8960soundcard"};
  std::filesystem::path sound{"/usr/local/share/porchlight/chime.wav"};
};

struct SpoolConfig {
  std::filesystem::path path{"/var/lib/porchlight/spool"};
  std::uintmax_t max_bytes{512ull * 1024 * 1024};
};

struct RecorderConfig {
  std::string video_source{"test"};  // test | libcamera
  std::string audio_source{"test"};  // test | alsa
  std::string audio_device{"hw:CARD=wm8960soundcard"};
  std::string encoder{"x264"};  // x264 | v4l2
  // Probed at startup: which AAC encoder exists differs between Pi OS and WSL.
  std::string aac_element{"avenc_aac"};
  int width{640};
  int height{480};
  int fps{30};
  int video_bitrate_kbps{2000};
};

struct GpioConfig {
  std::string chip{"/dev/gpiochip0"};
  // BCM numbering, and confirmed against `gpioinfo` on the real Pi - these are
  // not guesses any more. They also avoid GPIO 2-3 and 18-21, which the WM8960
  // HAT holds for its control I2C and its I2S.
  int button_line{24};
  int motion_line{23};
  int led_line{25};
  // Which way each line reads. The button is a switch to ground with the Pi's
  // internal pull-up, so it rests high and a press is low; the AM312 PIR and
  // the LED are both the plain way round.
  bool button_active_low{true};
  bool motion_active_low{false};
  bool led_active_low{false};
  // Milliseconds, and hardware: contact bounce, not the press de-dup window.
  // Applied by the kernel to the button only - a PIR has nothing to bounce.
  int debounce_ms{30};
};

struct Config {
  std::string device_id;
  Level log_level{Level::Info};
  Backends backends;
  Policy policy;
  ServerConfig server;
  ChimeConfig chime;
  SpoolConfig spool;
  RecorderConfig recorder;
  MediaConfig media;
  GpioConfig gpio;
};

Config load_config(const std::filesystem::path& path);

}  // namespace porch
