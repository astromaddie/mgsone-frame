#pragma once
#include "eye_view.h"
#include "runtime/gpu/modern_replay.h"
#include <optional>
#include <cstdint>
#include <map>
#include <cstring>
#include <algorithm>

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
  return !packet.originalMesh && packet.surfaceEffect.kind != replay::MgsSurfaceKind::Panorama &&
         (packet.route == replay::PacketRoute::Screen2D || packet.validMask == 0);
}

// Native coordinates already contain drawOffset. Require the actual uniform
// rectangle and full draw page, not a bounding box around a partial/diagonal quad.
inline bool mgsFullPageRectangle(const replay::DrawPacket &packet,
                                uint32_t pageX, uint32_t width, uint32_t height) {
  if (!isMgsHudCandidate(packet) || packet.vertexCount != 4 || packet.textured ||
      packet.gouraud || packet.state.mask.forceMaskBit || packet.state.mask.checkMaskBit ||
      packet.surfaceEffect.kind != replay::MgsSurfaceKind::None || !width || !height)
    return false;
  const auto &a = packet.state.drawArea;
  if (a.left != int32_t(pageX) || a.top != 0 ||
      a.right != int32_t(pageX + width - 1) || a.bottom != int32_t(height - 1))
    return false;
  const auto &v = packet.vertices;
  for (unsigned i = 1; i < 4; ++i)
    if (v[i].color != v[0].color) return false;
  return v[0].nativeX == v[2].nativeX && v[1].nativeX == v[3].nativeX &&
         v[0].nativeY == v[1].nativeY && v[2].nativeY == v[3].nativeY &&
         v[0].nativeX <= int32_t(pageX) && v[1].nativeX >= int32_t(pageX + width) &&
         v[0].nativeY <= 0 && v[2].nativeY >= int32_t(height);
}

// DRAWENV clears supply the native scene background, not a floating HUD plate.
inline bool isMgsHudBackground(const replay::DrawPacket &packet,
                                uint32_t pageX, uint32_t width, uint32_t height) {
  return (packet.opcode == 0x60 || packet.opcode == 0x28) &&
         !packet.state.blend.enabled && packet.screenEffect.kind == replay::MgsScreenEffectKind::None &&
         mgsFullPageRectangle(packet, pageX, width, height);
}

// Full-page untextured blends affect the entire eye. Small radar/menu panels
// retain their HUD placement and existing textured/STP rendering path.
inline bool isMgsEyeOverlay(const replay::DrawPacket &packet,
                            uint32_t pageX, uint32_t width, uint32_t height) {
  return (packet.opcode == 0x62 || packet.opcode == 0x2a) && packet.state.blend.enabled &&
         mgsFullPageRectangle(packet, pageX, width, height);
}
inline Vec3 mgsEyeOverlayPosition(double x, double y, uint32_t pageX,
                                  uint32_t width, uint32_t height) {
  // Positive Vulkan viewport height: clip Y=-1 is the top of the eye.
  return {float(2 * (x - pageX) / width - 1), float(2 * y / height - 1), 0};
}
struct MgsEyeOverlayBlend {
  float foreground, background;
  bool subtract;
};
inline MgsEyeOverlayBlend mgsEyeOverlayBlend(replay::BlendMode mode) {
  switch (mode) {
    case replay::BlendMode::HalfBackgroundPlusHalfForeground: return {0.5f, 0.5f, false};
    case replay::BlendMode::BackgroundPlusForeground: return {1, 1, false};
    case replay::BlendMode::BackgroundMinusForeground: return {1, 1, true};
    case replay::BlendMode::BackgroundPlusQuarterForeground: return {0.25f, 1, false};
  }
  return {1, 1, false};
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

// The renderer and diagnostics share this original-corner transform path.
inline bool sourceMeshPosition(std::span<const uint8_t> bytes,
                  const replay::OriginalMeshGeometry &mesh,
                  unsigned lane, Vec3 &out, bool widescreen,
                  const std::map<uint32_t, Matrix> *changes = nullptr) {
  Vec3 eye{};
  for (unsigned corner = 0; corner < 4; ++corner) {
    const float weight = mesh.weights[lane][corner];
    if (weight == 0) continue;
    const uint32_t reference = mesh.pointBytes[corner];
    const bool packed = reference >> 31;
    const size_t offset = reference & 0x7fffffffu;
    if (offset > bytes.size() || bytes.size() - offset < (packed ? 6u : 12u))
      return false;
    Vec3 point{};
    for (unsigned axis = 0; axis < 3; ++axis) {
      if (packed) {
        int16_t value;
        std::memcpy(&value, bytes.data() + offset + axis * 2, 2);
        point[axis] = value;
      } else {
        std::memcpy(&point[axis], bytes.data() + offset + axis * 4, 4);
      }
    }
    Vec3 captured{};
    for (unsigned row = 0; row < 3; ++row) {
      const auto &r = mesh.viewRows[corner * 3 + row];
      captured[row] = (point[0]*r[0]+point[1]*r[1]+point[2]*r[2]) + r[3];
    }
    if (changes) {
      const auto change = changes->find(mesh.vrPoseOwners[corner]);
      if (change != changes->end()) {
        captured[0] /= widescreen ? 0.75f : 1.0f;
        captured[1] /= 58.0f / 64.0f;
        const auto native = worldFromView(mesh.temporalCamera, captured);
        if (!native) return false;
        const auto transform = [](const Matrix &m, Vec3 p) {
          Vec3 result{};
          for (unsigned r = 0; r < 3; ++r)
            result[r] = m[12+r]+m[r]*p[0]+m[4+r]*p[1]+m[8+r]*p[2];
          return result;
        };
        captured = transform(mesh.temporalCamera, transform(change->second, *native));
        captured[0] *= widescreen ? 0.75f : 1.0f;
        captured[1] *= 58.0f / 64.0f;
      }
    }
    for (unsigned row = 0; row < 3; ++row) eye[row] += weight * captured[row];
  }
  eye[0] /= widescreen ? 0.75f : 1.0f;
  eye[1] /= 58.0f / 64.0f;
  const auto world = worldFromView(mesh.temporalCamera, eye);
  if (!world) return false;
  out = *world;
  return std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); });
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
