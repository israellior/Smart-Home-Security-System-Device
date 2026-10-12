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

// Which implementation each interface gets. Each has one real backend now;
// the names stay so a third input - a kernel driver, say - is one more branch.
struct Backends {
  std::string input{"gpio"};
  std::string led{"gpio"};
  std::string chime{"alsa"};
  std::string server{"bridge"};
  std::string uploader{"script"};
  std::string media{"script"};
  std::string recorder{"gstreamer"};
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
  // How long to wait before starting the bridge again after it has ended, and
  // the ceiling that wait climbs to. The bridge reconnects by itself, so it
  // only exits when something is wrong with the bridge rather than with the
  // network - a missing interpreter, an unreadable credential, a crash - and
  // every one of those fails again instantly. Without a wait between attempts
  // the daemon would spend a whole core forking a process that dies on exec.
  std::chrono::seconds restart_backoff_initial{2};
  std::chrono::seconds restart_backoff_max{60};
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
  // How long the call keeps trying to rejoin LiveKit before it gives up and
  // exits. Longer than the daemon's own reconnect budget on purpose: a call is
  // something a person is watching right now, and one that survives a lift
  // doors' worth of bad signal is worth more than one that ends tidily.
  std::chrono::seconds reconnect_timeout{60};
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

// The person detector, which runs inside the recorder on a second branch of
// the same camera - only one process can hold the camera, so it cannot open
// its own. The timings it shares with the core live in Policy.
struct DetectConfig {
  bool enabled{false};
  // The inference runtime is a pip package, so the recorder runs under the
  // venv's interpreter when it detects, and under the system one otherwise.
  std::filesystem::path python{"/opt/porchlight/venv/bin/python3"};
  // A TFLite SSD detector with the standard four-output post-processing -
  // COCO SSD MobileNet, or EfficientDet-Lite - where class 0 is a person.
  std::filesystem::path model{"/usr/local/share/porchlight/person.tflite"};
  // The model's square input, which GStreamer scales to. A multiple of four,
  // so an RGB row has no padding and the frame is exactly size*size*3 bytes.
  int input_size{300};
  // Frames handed to the detector per second. More than the Pi can infer is
  // harmless - the branch drops what the detector is too slow to take.
  int fps{5};
  int min_score_percent{50};
  // Average brightness, out of 255, below which a frame is too dark to judge.
  // The camera has no infrared, so at night every frame is below it and every
  // motion alerts unjudged, as it did before.
  int min_luma{40};
  // Bright frames that must have been looked at before "nobody" is believed.
  // Fewer, and the answer is unknown, which alerts.
  int min_frames{8};
};

struct RecorderConfig {
  // gst-launch-1.0 -e with the clock pinned; see pi/record-clip.py for why
  // gst-launch itself cannot record the camera and the card together.
  std::filesystem::path script{"/usr/local/lib/porchlight/record-clip.py"};
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
  DetectConfig detect;
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
