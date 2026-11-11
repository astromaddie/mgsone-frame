#pragma once
#include <array>
#include <cstdint>
struct recomp_context;
namespace ps1 { class Memory; }
namespace ps1::games::mgs {
// Publish the tracked controller's world direction. A zero vector clears it.
void setVrShotDirection(const std::array<float, 3> &direction) noexcept;
void applyVrFacing(Memory &memory, uint16_t heading, const int16_t *aimPitch = nullptr);
}
void mgsApplyVrBulletAim(recomp_context *ctx);
