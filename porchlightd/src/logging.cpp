#include "logging.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace porch {
namespace {

Level g_min_level = Level::Info;

const char* name_of(Level level) {
  switch (level) {
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
  }
  return "?";
}

// Built by hand rather than with chrono's formatter, so it behaves identically
// on GCC 13 (WSL) and GCC 14 (Pi OS trixie).
std::string timestamp() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto whole_seconds = floor<seconds>(now);
  const auto ms = duration_cast<milliseconds>(now - whole_seconds).count();

  const std::time_t as_time_t = system_clock::to_time_t(whole_seconds);
  std::tm utc{};
  ::gmtime_r(&as_time_t, &utc);

  char buffer[32];
  const std::size_t len = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc);
  return std::format("{}.{:03d}Z", std::string_view(buffer, len), ms);
}

}  // namespace

void set_min_level(Level level) { g_min_level = level; }

bool level_enabled(Level level) { return level >= g_min_level; }

void write_line(Level level, std::string_view subsystem, std::string_view text) {
  // One line per event on stderr, which is where journald collects it.
  const std::string line =
      std::format("{} {:<5} {:<9} {}\n", timestamp(), name_of(level), subsystem, text);
  std::fwrite(line.data(), 1, line.size(), stderr);
}

}  // namespace porch
