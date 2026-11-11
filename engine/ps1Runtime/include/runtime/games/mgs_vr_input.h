#pragma once
#include <array>
#include <cstdint>
#include "runtime/input/vr_input.h"
struct recomp_context;
namespace ps1 { class Memory; }
namespace ps1::games::mgs {
// Shared by skeletal capture and the guest weapon bridge. Movement admission
// remains separate: a native firing action must not become VR locomotion.
bool isVrFirearmAction(const Memory &memory, uint32_t handler,
                       int16_t weapon) noexcept;
// Publish the tracked controller's world direction. A zero vector clears it.
void setVrShotDirection(const std::array<float, 3> &direction) noexcept;
// The renderer publishes only a desired wrist transform. The guest NewBullet
// boundary validates ownership and the native hazard segment before using it.
struct VrWeaponPose {
  uint32_t wristOwner = 0;
  std::array<float, 16> wristWorld{};
  std::array<std::array<float, 16>, 3> armWorld{};
};
void setVrWeaponPose(const VrWeaponPose &pose) noexcept;
struct VrWeaponConstraint {
  VrWeaponPose pose;
  bool nativeFallback = false;
};
VrWeaponConstraint vrWeaponConstraint() noexcept;
// Atomically publish a sample. Only guest-thread bridges may touch actor RAM.
void setVrMovement(const input::VrInputSample &sample) noexcept;
}
void mgsApplyVrBulletAim(recomp_context *ctx);

void mgsApplyVrLocomotion(recomp_context *ctx);
void mgsApplyVrControl(recomp_context *ctx);
