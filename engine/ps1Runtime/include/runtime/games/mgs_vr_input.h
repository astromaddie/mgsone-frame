#pragma once
#include <array>
#include <cstdint>
#include "runtime/input/vr_input.h"
struct recomp_context;
namespace ps1 { class Memory; }
namespace ps1::games::mgs {
// Publish the tracked controller's world direction. A zero vector clears it.
void setVrShotDirection(const std::array<float, 3> &direction) noexcept;
// Atomically publish a sample. Only guest-thread bridges may touch actor RAM.
void setVrMovement(const input::VrInputSample &sample) noexcept;
}
void mgsApplyVrBulletAim(recomp_context *ctx);

void mgsApplyVrLocomotion(recomp_context *ctx);
void mgsApplyVrControl(recomp_context *ctx);
