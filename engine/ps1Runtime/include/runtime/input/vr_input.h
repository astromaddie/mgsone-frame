#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>

namespace ps1::input {
struct VrInputSample {
  uint16_t buttons = 0;
  bool analogMovement = false;
  uint8_t x = 128, y = 128;
  std::optional<uint16_t> facing;
};

inline std::array<uint8_t, 2> headRelativeStick(float right, float forward,
                                             float worldHeading, uint16_t padOrigin) {
  const float magnitude = std::min(1.0f, std::hypot(right, forward));
  if (magnitude < 0.25f) return {128, 128};
  const float angle = worldHeading - std::atan2(right, forward) -
      padOrigin * (2 * std::numbers::pi_v<float> / 4096);
  return {uint8_t(std::lround(128 + 127 * magnitude * std::sin(angle))),
          uint8_t(std::lround(128 + 127 * magnitude * std::cos(angle)))};
}
}
