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
  int button_line{17};
  int motion_line{27};
  int led_line{22};
  // Milliseconds, and hardware: contact bounce, not the press de-dup window.
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
  GpioConfig gpio;
};

Config load_config(const std::filesystem::path& path);

}  // namespace porch
