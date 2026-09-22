#pragma once

#include <chrono>
#include <ctime>
#include <format>
#include <string>
#include <string_view>

namespace porch {

// UTC with milliseconds, which is what the server's `at` field expects and
// what the log lines use. Built by hand rather than with chrono's formatter so
// it behaves identically on GCC 13 (WSL) and GCC 14 (Pi OS trixie).
inline std::string iso8601(std::chrono::system_clock::time_point when) {
  using namespace std::chrono;
  const auto whole_seconds = floor<seconds>(when);
  const auto ms = duration_cast<milliseconds>(when - whole_seconds).count();

  const std::time_t as_time_t = system_clock::to_time_t(whole_seconds);
  std::tm utc{};
  ::gmtime_r(&as_time_t, &utc);

  char buffer[32];
  const std::size_t len = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc);
  return std::format("{}.{:03d}Z", std::string_view(buffer, len), ms);
}

}  // namespace porch
