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
  SpoolConfig spool;
  RecorderConfig recorder;
  GpioConfig gpio;
};

Config load_config(const std::filesystem::path& path);

}  // namespace porch
