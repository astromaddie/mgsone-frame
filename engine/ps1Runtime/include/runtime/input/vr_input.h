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
  std::optional<uint16_t> movementHeading;
  std::optional<int16_t> aimPitch;
};

// View remains Select on a short RELEASE. Holding it consumes Select and
// emits one recenter event. A 50ms pulse crosses every 60Hz guest latch phase
// at 72/90/120Hz presentation without depending on a single XR frame.
struct VrViewButton {
  struct Result { bool select = false, recenter = false; };
  static constexpr int64_t holdTime = 1000000000, pulseTime = 50000000;
  bool armed = false, held = false, longFired = false, haveTime = false;
  int64_t pressedAt = 0, pulseUntil = 0, lastTime = 0;
  void reset() { *this = {}; }
  Result sample(bool down, bool focused, int64_t predictedTime) {
    if (!focused || (haveTime && predictedTime < lastTime)) { reset(); return {}; }
    haveTime = true; lastTime = predictedTime;
    Result result;
    if (!armed) {
      if (!down) armed = true;
      return result;
    }
    if (down && !held) { held = true; longFired = false; pressedAt = predictedTime; }
    if (held && !longFired && predictedTime - pressedAt >= holdTime) {
      longFired = true;
      result.recenter = true;
    }
    if (!down && held) {
      if (!longFired) pulseUntil = predictedTime + pulseTime;
      held = false;
    }
    result.select = predictedTime < pulseUntil;
    return result;
  }
};

inline uint16_t vrHeading(float angle) {
  return uint16_t(int32_t(std::lround(angle * (4096 / (2 * std::numbers::pi_v<float>)))) & 4095);
}

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
