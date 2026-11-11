#pragma once
#include "eye_view.h"
#include "runtime/gpu/modern_replay.h"
#include <optional>
#include <cstdint>

namespace ps1::gpu::vr {
// A live gameplay HUD shares head translation, yaw and pitch, never roll.
// This is deliberately separate from the fixed cinema/menu anchor.
inline Anchor hudAnchor(const std::array<EyeView, 2> &eyes) {
  Anchor result = recenter(eyes);
  const Vec3 forward = rotate(eyes[0].orientation, {0, 0, -1});
  const float pitch = std::atan2(forward[1], std::hypot(forward[0], forward[2]));
  const float sp = std::sin(pitch * 0.5f), cp = std::cos(pitch * 0.5f);
  const float sy = result.heading[1], cy = result.heading[3];
  result.heading = {cy*sp, sy*cp, -sy*sp, cy*cp};
  return result;
}

// Changing the tracking origin must not change the current world heading.
// Transfer head/stick yaw into the camera basis before resetting both deltas.
inline void recenterFirstPerson(Anchor &anchor, const std::array<EyeView, 2> &eyes,
                                float &baseHeading, float &stickTurn, float &headTurn) {
  const Anchor next = recenter(eyes);
  const float oldYaw = 2*std::atan2(anchor.heading[1], anchor.heading[3]);
  const float newYaw = 2*std::atan2(next.heading[1], next.heading[3]);
  // Use the actual anchor change, including a vertical-look fallback, rather
  // than a potentially stale head delta from the previous display frame.
  baseHeading += newYaw - oldYaw - stickTurn;
  stickTurn = headTurn = 0;
  anchor = next;
}

inline bool isSourceWorldFace(const replay::DrawPacket &packet) {
  return packet.originalMesh && packet.originalMesh->sourceTopology &&
         packet.route == replay::PacketRoute::Projected3D;
}

enum class MgsPresentation { Reuse, Gameplay, Cinema };
inline MgsPresentation mgsPresentation(std::span<const replay::Operation> operations,
                                       const std::optional<replay::MgsPlayerView> &player) {
  bool source = false, gameplay = false;
  for (const auto &op : operations) {
    const auto *packet = std::get_if<replay::DrawPacket>(&op);
    if (!packet) continue;
    if (packet->screenEffect.kind == replay::MgsScreenEffectKind::CodecScreen)
      return MgsPresentation::Cinema;
    if (!isSourceWorldFace(*packet)) continue;
    source = true;
    // Source geometry also exists in title/menu scenes. The producer's
    // authenticated live Snake/body witness distinguishes playable world
    // snapshots and already rejects authored views, scripts and pause.
    gameplay |= player && packet->originalMesh->temporalIdentity[0] == player->sceneEpoch;
  }
  if (gameplay) return MgsPresentation::Gameplay;
  // An explicit non-gameplay scene replaces the world immediately. Empty
  // latches between 20/30Hz scene submissions may reuse its previous snapshot.
  return source ? MgsPresentation::Cinema : MgsPresentation::Reuse;
}

// MGS's menu draw environments are horizontal 320-pixel pages at X=0/320.
// A radar draw area is a subrect of that page, with its own local draw offset.
// Preserve an enclosing page witness when an animated subrect crosses its edge.
inline std::optional<uint32_t> mgsHudPage(const replay::DrawArea &area,
                                         std::optional<uint32_t> previous = {}) {
  if (area.right < area.left || area.bottom < area.top || area.top < 0 || area.bottom >= 256)
    return {};
  if (area.left >= 0 && area.right < 320) return 0;
  if (area.left >= 320 && area.right < 640) return 320;
  if (previous && area.left < int32_t(*previous + 320) && area.right >= int32_t(*previous))
    return previous;
  return {};
}

inline bool isMgsHudCandidate(const replay::DrawPacket &packet) {
  return !packet.originalMesh &&
         (packet.route == replay::PacketRoute::Screen2D || packet.validMask == 0);
}

// MGS's background DRAWENV clears can arrive as TILE/POLY_F4 DrawPackets,
// not just FillRect operations. A flat opaque rectangle covering the complete
// draw page supplies the native scene background, not a floating HUD plate.
// Keep textured panels, radar subrects and semi-transparent fades intact.
inline bool isMgsHudBackground(const replay::DrawPacket &packet,
                                uint32_t pageX, uint32_t width, uint32_t height) {
  if (!isMgsHudCandidate(packet) || (packet.opcode != 0x60 && packet.opcode != 0x28) ||
      packet.vertexCount != 4 || packet.textured || packet.gouraud ||
      packet.state.blend.enabled || packet.state.mask.forceMaskBit || packet.state.mask.checkMaskBit ||
      packet.screenEffect.kind != replay::MgsScreenEffectKind::None ||
      packet.surfaceEffect.kind != replay::MgsSurfaceKind::None || width == 0 || height == 0)
    return false;
  const auto &a = packet.state.drawArea;
  if (a.left != int32_t(pageX) || a.top != 0 ||
      a.right != int32_t(pageX + width - 1) || a.bottom != int32_t(height - 1))
    return false;
  const auto &v = packet.vertices;
  for (unsigned i = 1; i < 4; ++i)
    if (v[i].color != v[0].color) return false;
  // Native coordinates already contain drawOffset. Test the actual rectangle,
  // not a bounding box that could also describe a diagonal or partial quad.
  return v[0].nativeX == v[2].nativeX && v[1].nativeX == v[3].nativeX &&
         v[0].nativeY == v[1].nativeY && v[2].nativeY == v[3].nativeY &&
         v[0].nativeX <= int32_t(pageX) && v[1].nativeX >= int32_t(pageX + width) &&
         v[0].nativeY <= 0 && v[2].nativeY >= int32_t(height);
}

// Invert the retained channel transform, including its Q12 rounding. The
// channel is a witness of how a face was captured, never the room camera.
inline std::optional<Vec3> worldFromView(const Matrix &m, Vec3 p) {
  for (unsigned i = 0; i < 3; ++i) p[i] -= m[12 + i];
  const Vec3 a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
  const auto cross = [](Vec3 x, Vec3 y) {
    return Vec3{x[1]*y[2]-x[2]*y[1], x[2]*y[0]-x[0]*y[2], x[0]*y[1]-x[1]*y[0]};
  };
  const auto dot = [](Vec3 x, Vec3 y) { return x[0]*y[0]+x[1]*y[1]+x[2]*y[2]; };
  const auto bc = cross(b,c);
  const float det = dot(a,bc);
  if (!std::isfinite(det) || std::abs(det) < 1e-6f) return {};
  return Vec3{dot(p,bc)/det, dot(p,cross(c,a))/det, dot(p,cross(a,b))/det};
}

struct Toybox {
  uint64_t epoch = 0;
  bool placed = false;
  Vec3 centre{};
  // A 1.8m character becomes a 14cm figure. Placement changes only when a
  // different scene loads, not when the player walks or the channel cuts.
  static constexpr float scale = 0.08f / 1000.0f;
  void place(uint64_t scene, Vec3 origin) {
    if (placed && epoch == scene) return;
    epoch = scene;
    centre = origin;
    placed = true;
  }
  Vec3 transform(Vec3 p) const {
    return {-(p[0]-centre[0])*scale, (p[1]-centre[1])*scale-0.9f,
            -(p[2]-centre[2])*scale-2.4f};
  }
};
}
