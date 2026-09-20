#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace porch {

enum class Level { Debug, Info, Warn, Error };

void set_min_level(Level level);
bool level_enabled(Level level);

void write_line(Level level, std::string_view subsystem, std::string_view text);

template <typename... Args>
void log(Level level, std::string_view subsystem, std::format_string<Args...> fmt, Args&&... args) {
  if (!level_enabled(level)) {
    return;
  }
  write_line(level, subsystem, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace porch
