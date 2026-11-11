#include "runtime/games/mgs_vr_input.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

namespace {
std::atomic<uint64_t> vrShotDirection{0}, vrMovement{0};
constexpr uint64_t movementActive = uint64_t(1) << 63, moveValid = uint64_t(1) << 62, pitchValid = uint64_t(1) << 61;

using ShotVector = std::array<float, 3>;
float shotDot(const ShotVector &a, const ShotVector &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
ShotVector shotCross(const ShotVector &a, const ShotVector &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}
}

namespace ps1::games::mgs {
void setVrMovement(const input::VrInputSample &sample) noexcept {
  uint64_t packed = 0;
  if (sample.facing) {
    packed = movementActive | (*sample.facing & 4095u);
    if (sample.movementHeading) packed |= moveValid | (uint64_t(*sample.movementHeading & 4095u) << 12);
    if (sample.aimPitch) packed |= pitchValid | (uint64_t(uint16_t(*sample.aimPitch)) << 24);
  }
  vrMovement.store(packed, std::memory_order_release);
}

void setVrShotDirection(const std::array<float, 3> &direction) noexcept {
  const float length = std::sqrt(shotDot(direction, direction));
  uint64_t packed = 0;
  if (std::isfinite(length) && length > 0.01f) {
    for (unsigned axis = 0; axis < 3; ++axis) {
      const auto value = int16_t(std::lround(direction[axis] * (4096.0f / length)));
      packed |= uint64_t(uint16_t(value)) << (axis * 16);
    }
  }
  vrShotDirection.store(packed, std::memory_order_relaxed);
}
}

namespace {
bool vrSnake(ps1::Memory &memory, uint32_t actor, uint64_t sample) {
  if (!(sample & movementActive) || actor < 0x80010000u || actor > 0x801FF500u ||
      memory.read32(0x800AE16Cu) != actor + 0x20 || memory.read32(0x800AE0A0u) != 0 ||
      (memory.read32(0x800ADB3Cu) & 0xdc080050u) ||
      (memory.read32(0x800AE1C8u) & ((sample & pitchValid) ? 0x20007282u : 0x20007283u))) return false;
  const auto name = memory.read32(actor + 20);
  if (name < 0x80010000u || name > 0x801FFFF8u ||
      std::memcmp(memory.ramPtr() + (name & 0x1fffffffu), "snake.c", sizeof("snake.c"))) return false;
  // USA idle/crouch/prone/run and run-start handlers. Damage, wall presses,
  // ladders, dragging, scripted motion and weapon-specific stances stay native.
  const uint32_t handler = memory.read32(actor + 0x9c4);
  return handler == 0x80054854u || handler == 0x800549D8u || handler == 0x80054B50u ||
         handler == 0x80054C6Cu || handler == 0x80054E80u || handler == 0x80055B6Cu;
}
void vrFacing(ps1::Memory &memory, uint32_t control, uint64_t sample) {
  const uint16_t heading = sample & 4095u;
  memory.write16(control + 10, heading);
  memory.write16(control + 0x4e, heading);
  if (sample & pitchValid) {
    const uint16_t pitch = sample >> 24;
    memory.write16(control + 8, pitch);
    memory.write16(control + 0x4c, pitch);
  }
}
}

void mgsApplyVrLocomotion(recomp_context *ctx) {
  if (!ctx || !ctx->mem) return;
  const auto sample = vrMovement.load(std::memory_order_acquire);
  const uint32_t actor = ctx->r18; // USA main logic keeps Snake in s2.
  auto &memory = *ctx->mem;
  if (!vrSnake(memory, actor, sample) || ctx->r28 + 0x920 < 0x80010000u ||
      ctx->r28 + 0x920 > 0x801FFFFCu) return;
  // The original pad and wall-direction checks have already run. Let the
  // normal animation state machine see movement along the facing direction,
  // avoiding its turn-around/start delay for backwards and lateral input.
  const uint32_t direction = ctx->r28 + 0x920;
  if (int32_t(memory.read32(direction)) >= 0 && (sample & moveValid))
    memory.write32(direction, sample & 4095u);
  vrFacing(memory, actor + 0x20, sample);
}

void mgsApplyVrControl(recomp_context *ctx) {
  // This is the actual Snake call to GM_ActControl, after root animation has
  // produced step and before CheckCollide/CheckHeight consume it.
  if (!ctx || !ctx->mem || ctx->r31 != 0x8005D0FCu) return;
  const auto sample = vrMovement.load(std::memory_order_acquire);
  const auto control = ctx->r4;
  auto &memory = *ctx->mem;
  if (!vrSnake(memory, control - 0x20, sample)) return;
  if (sample & moveValid) {
    const float x = int16_t(memory.read16(control + 0x44));
    const float z = int16_t(memory.read16(control + 0x48));
    const float speed = std::hypot(x, z);
    const float angle = float((sample >> 12) & 4095u) * (2 * 3.14159265358979323846f / 4096);
    memory.write16(control + 0x44, uint16_t(int16_t(std::lround(std::sin(angle) * speed))));
    memory.write16(control + 0x48, uint16_t(int16_t(std::lround(std::cos(angle) * speed))));
  }
  vrFacing(memory, control, sample);
  ps1::metrics::trace("mgs.vr.control", {{"facing", int64_t(sample & 4095u)},
      {"move", (sample & moveValid) ? int64_t((sample >> 12) & 4095u) : -1},
      {"step_x", int16_t(memory.read16(control + 0x44))},
      {"step_z", int16_t(memory.read16(control + 0x48))}});
}

void mgsApplyVrBulletAim(recomp_context *ctx) {
  // These USA call sites pass a 32-byte shot matrix at caller sp + 0x10.
  // Retain every ammo, trigger, actor and collision rule in NewBullet.
  if (!ctx || !ctx->mem || ctx->r5 != 1 ||
      (ctx->r31 != 0x80067B7Cu && ctx->r31 != 0x800682D8u && ctx->r31 != 0x8006A2D4u))
    return;
  const uint64_t packed = vrShotDirection.load(std::memory_order_relaxed);
  const uint32_t matrix = ctx->r4;
  if (!packed || matrix != ctx->r29 + 16 || matrix < 0x80010000u || matrix > 0x801FFFE0u)
    return;
  auto &memory = *ctx->mem;
  std::array<ShotVector, 3> oldColumns{};
  ShotVector aim{};
  for (unsigned row = 0; row < 3; ++row) {
    aim[row] = -float(int16_t(packed >> (row * 16))) / 4096.0f;
    for (unsigned col = 0; col < 3; ++col)
      oldColumns[col][row] = int16_t(memory.read16(matrix + (row * 3 + col) * 2));
  }
  ShotVector right = oldColumns[0];
  const float vertical = shotDot(right, aim);
  for (unsigned axis = 0; axis < 3; ++axis) right[axis] -= vertical * aim[axis];
  float rightLength = std::sqrt(shotDot(right, right));
  if (rightLength < 1) {
    right = shotCross(aim, std::abs(aim[1]) < 0.9f ? ShotVector{0, 1, 0} : ShotVector{1, 0, 0});
    rightLength = std::sqrt(shotDot(right, right));
  }
  for (auto &component : right) component /= rightLength;
  const std::array columns{right, aim, shotCross(right, aim)};
  std::array<ShotVector, 3> scaled = columns;
  for (unsigned col = 0; col < 3; ++col) {
    const float length = std::sqrt(shotDot(oldColumns[col], oldColumns[col]));
    if (length < 1 || length > 8192) return;
    for (auto &component : scaled[col]) component *= length;
  }
  for (unsigned row = 0; row < 3; ++row) {
    // NewBullet starts at local (0, 400, 0) and travels down its -Y axis.
    const uint32_t position = matrix + 20 + row * 4;
    const auto correction = int32_t(std::lround((oldColumns[1][row] - scaled[1][row]) * (400.0f / 4096)));
    memory.write32(position, memory.read32(position) + uint32_t(correction));
    for (unsigned col = 0; col < 3; ++col)
      memory.write16(matrix + (row * 3 + col) * 2, uint16_t(int16_t(std::lround(scaled[col][row]))));
  }
  ps1::metrics::count("mgs.vr.aimed_shots");
  ps1::metrics::trace("mgs.vr.aimed_shot", {{"call", ctx->r31},
      {"x", -int32_t(std::lround(scaled[1][0]))}, {"y", -int32_t(std::lround(scaled[1][1]))},
      {"z", -int32_t(std::lround(scaled[1][2]))}});
}
