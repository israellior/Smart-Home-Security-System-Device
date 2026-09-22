#include "config.h"

#include <nlohmann/json.hpp>

#include <format>
#include <fstream>
#include <initializer_list>
#include <string_view>

namespace porch {
namespace {

using nlohmann::json;

const json& empty_object() {
  static const json value = json::object();
  return value;
}

// A node plus the dotted path that reached it, so every failure can name the
// key the reader was actually looking at.
class Section {
 public:
  Section(const json& node, std::string path) : node_(node), path_(std::move(path)) {}

  Section section(std::string_view key) const {
    const auto it = find(key);
    if (it == node_.end()) {
      return Section(empty_object(), path_of(key));
    }
    if (!it->is_object()) {
      fail(key, "an object");
    }
    return Section(*it, path_of(key));
  }

  std::string required_string(std::string_view key) const {
    const auto it = find(key);
    if (it == node_.end() || !it->is_string() || it->get<std::string>().empty()) {
      fail(key, "a non-empty string");
    }
    return it->get<std::string>();
  }

  std::string string_or(std::string_view key, std::string fallback) const {
    const auto it = find(key);
    if (it == node_.end()) {
      return fallback;
    }
    if (!it->is_string()) {
      fail(key, "a string");
    }
    return it->get<std::string>();
  }

  std::chrono::seconds seconds_or(std::string_view key, std::chrono::seconds fallback) const {
    const auto it = find(key);
    if (it == node_.end()) {
      return fallback;
    }
    if (!it->is_number_unsigned()) {
      fail(key, "a whole number of seconds, zero or more");
    }
    return std::chrono::seconds(it->get<std::uint64_t>());
  }

  int int_or(std::string_view key, int fallback) const {
    const auto it = find(key);
    if (it == node_.end()) {
      return fallback;
    }
    if (!it->is_number_integer()) {
      fail(key, "an integer");
    }
    return it->get<int>();
  }

  std::uintmax_t bytes_or(std::string_view key, std::uintmax_t fallback) const {
    const auto it = find(key);
    if (it == node_.end()) {
      return fallback;
    }
    if (!it->is_number_unsigned()) {
      fail(key, "a byte count, zero or more");
    }
    return it->get<std::uintmax_t>();
  }

  std::size_t count_or(std::string_view key, std::size_t fallback) const {
    return static_cast<std::size_t>(bytes_or(key, fallback));
  }

  std::string one_of(std::string_view key, std::string fallback,
                     std::initializer_list<std::string_view> allowed) const {
    std::string value = string_or(key, std::move(fallback));
    for (const std::string_view candidate : allowed) {
      if (value == candidate) {
        return value;
      }
    }
    std::string list;
    for (const std::string_view candidate : allowed) {
      list += list.empty() ? "" : ", ";
      list += candidate;
    }
    fail(key, std::format("one of: {}", list));
  }

  [[noreturn]] void fail(std::string_view key, std::string_view expected) const {
    throw ConfigError(std::format("{}: expected {}", path_of(key), expected));
  }

 private:
  json::const_iterator find(std::string_view key) const { return node_.find(std::string(key)); }

  std::string path_of(std::string_view key) const {
    return path_.empty() ? std::string(key) : std::format("{}.{}", path_, key);
  }

  const json& node_;
  std::string path_;
};

Level parse_level(const Section& root) {
  const std::string value = root.one_of("log_level", "info", {"debug", "info", "warn", "error"});
  if (value == "debug") return Level::Debug;
  if (value == "warn") return Level::Warn;
  if (value == "error") return Level::Error;
  return Level::Info;
}

void require_positive(std::string_view key, std::chrono::seconds value) {
  if (value <= std::chrono::seconds::zero()) {
    throw ConfigError(std::format("{}: expected more than zero seconds", key));
  }
}

void require_positive(std::string_view key, long long value) {
  if (value <= 0) {
    throw ConfigError(std::format("{}: expected more than zero", key));
  }
}

void validate(const Config& cfg) {
  const Policy& p = cfg.policy;
  require_positive("timings.clip_seconds", p.clip);
  require_positive("timings.alert_max_age_seconds", p.alert_max_age);
  require_positive("timings.retry_backoff_initial_seconds", p.retry_backoff_initial);
  require_positive("timings.max_queued_alerts", static_cast<long long>(p.max_queued_alerts));

  if (p.min_clip > p.clip) {
    throw ConfigError("timings.min_clip_seconds: expected no more than timings.clip_seconds");
  }
  if (p.retry_backoff_max < p.retry_backoff_initial) {
    throw ConfigError(
        "timings.retry_backoff_max_seconds: expected at least "
        "timings.retry_backoff_initial_seconds");
  }

  require_positive("recorder.width", cfg.recorder.width);
  require_positive("recorder.height", cfg.recorder.height);
  require_positive("recorder.fps", cfg.recorder.fps);
  require_positive("recorder.video_bitrate_kbps", cfg.recorder.video_bitrate_kbps);
  require_positive("spool.max_bytes", static_cast<long long>(cfg.spool.max_bytes));

  if (cfg.spool.path.empty()) {
    throw ConfigError("spool.path: expected a non-empty path");
  }
}

}  // namespace

Config load_config(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    throw ConfigError(std::format("{}: cannot be opened", path.string()));
  }

  json doc;
  try {
    doc = json::parse(file);
  } catch (const json::parse_error& error) {
    throw ConfigError(std::format("{}: {}", path.string(), error.what()));
  }
  if (!doc.is_object()) {
    throw ConfigError(std::format("{}: expected a JSON object at the top level", path.string()));
  }

  const Section root(doc, "");
  Config cfg;
  cfg.device_id = root.required_string("device_id");
  cfg.log_level = parse_level(root);

  const Section backends = root.section("backends");
  cfg.backends.input = backends.string_or("input", cfg.backends.input);
  cfg.backends.led = backends.string_or("led", cfg.backends.led);
  cfg.backends.chime = backends.string_or("chime", cfg.backends.chime);
  cfg.backends.server = backends.string_or("server", cfg.backends.server);
  cfg.backends.uploader = backends.string_or("uploader", cfg.backends.uploader);
  cfg.backends.media = backends.string_or("media", cfg.backends.media);
  cfg.backends.recorder = backends.string_or("recorder", cfg.backends.recorder);

  const Section timings = root.section("timings");
  Policy& p = cfg.policy;
  p.clip = timings.seconds_or("clip_seconds", p.clip);
  p.min_clip = timings.seconds_or("min_clip_seconds", p.min_clip);
  p.motion_cooldown = timings.seconds_or("motion_cooldown_seconds", p.motion_cooldown);
  p.press_dedup_window = timings.seconds_or("press_dedup_window_seconds", p.press_dedup_window);
  p.ring_led = timings.seconds_or("ring_led_seconds", p.ring_led);
  p.alert_max_age = timings.seconds_or("alert_max_age_seconds", p.alert_max_age);
  p.max_queued_alerts = timings.count_or("max_queued_alerts", p.max_queued_alerts);
  p.retry_backoff_initial =
      timings.seconds_or("retry_backoff_initial_seconds", p.retry_backoff_initial);
  p.retry_backoff_max = timings.seconds_or("retry_backoff_max_seconds", p.retry_backoff_max);

  const Section server = root.section("server");
  cfg.server.base_url = server.string_or("base_url", cfg.server.base_url);
  cfg.server.credential_path =
      server.string_or("credential_path", cfg.server.credential_path.string());
  cfg.server.bridge_path = server.string_or("bridge_path", cfg.server.bridge_path.string());
  cfg.server.ack_timeout = server.seconds_or("ack_timeout_seconds", cfg.server.ack_timeout);

  const Section chime = root.section("chime");
  cfg.chime.device = chime.string_or("device", cfg.chime.device);
  cfg.chime.sound = chime.string_or("sound", cfg.chime.sound.string());

  const Section spool = root.section("spool");
  cfg.spool.path = spool.string_or("path", cfg.spool.path.string());
  cfg.spool.max_bytes = spool.bytes_or("max_bytes", cfg.spool.max_bytes);

  const Section recorder = root.section("recorder");
  cfg.recorder.video_source = recorder.one_of("video_source", cfg.recorder.video_source,
                                              {"test", "libcamera"});
  cfg.recorder.audio_source = recorder.one_of("audio_source", cfg.recorder.audio_source,
                                              {"test", "alsa"});
  cfg.recorder.audio_device = recorder.string_or("audio_device", cfg.recorder.audio_device);
  cfg.recorder.encoder = recorder.one_of("encoder", cfg.recorder.encoder, {"x264", "v4l2"});
  cfg.recorder.aac_element = recorder.string_or("aac_element", cfg.recorder.aac_element);
  cfg.recorder.width = recorder.int_or("width", cfg.recorder.width);
  cfg.recorder.height = recorder.int_or("height", cfg.recorder.height);
  cfg.recorder.fps = recorder.int_or("fps", cfg.recorder.fps);
  cfg.recorder.video_bitrate_kbps =
      recorder.int_or("video_bitrate_kbps", cfg.recorder.video_bitrate_kbps);

  const Section gpio = root.section("gpio");
  cfg.gpio.chip = gpio.string_or("chip", cfg.gpio.chip);
  cfg.gpio.button_line = gpio.int_or("button_line", cfg.gpio.button_line);
  cfg.gpio.motion_line = gpio.int_or("motion_line", cfg.gpio.motion_line);
  cfg.gpio.led_line = gpio.int_or("led_line", cfg.gpio.led_line);
  cfg.gpio.debounce_ms = gpio.int_or("debounce_ms", cfg.gpio.debounce_ms);

  validate(cfg);
  return cfg;
}

}  // namespace porch
