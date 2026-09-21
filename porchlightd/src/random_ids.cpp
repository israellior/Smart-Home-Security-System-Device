#include "random_ids.h"

#include <array>
#include <cstdint>
#include <format>

namespace porch {

RandomIds::RandomIds() {
  std::random_device device;
  // Two draws, because random_device yields 32 bits at a time here.
  const std::uint64_t seed =
      (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
  engine_.seed(seed);
}

EventId RandomIds::next() {
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < bytes.size(); i += 8) {
    const std::uint64_t chunk = engine_();
    for (std::size_t b = 0; b < 8; ++b) {
      bytes[i + b] = static_cast<std::uint8_t>(chunk >> (b * 8));
    }
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);  // variant 1

  std::string text;
  text.reserve(36);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      text.push_back('-');
    }
    text += std::format("{:02x}", bytes[i]);
  }
  return text;
}

}  // namespace porch
