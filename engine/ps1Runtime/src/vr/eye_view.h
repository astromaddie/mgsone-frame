#pragma once

#include <array>
#include <cmath>

namespace ps1::gpu::vr {
using Vec3 = std::array<float, 3>;
using Quaternion = std::array<float, 4>; // x, y, z, w
using Matrix = std::array<float, 16>;   // column major

struct EyeView {
  Vec3 position{};
  Quaternion orientation{0, 0, 0, 1};
  float left = -0.75f, right = 0.75f, down = -0.75f, up = 0.75f;
};

inline Quaternion inverse(Quaternion q) {
  return {-q[0], -q[1], -q[2], q[3]};
}

inline Vec3 rotate(const Quaternion &q, const Vec3 &v) {
  const Vec3 t{2 * (q[1] * v[2] - q[2] * v[1]),
               2 * (q[2] * v[0] - q[0] * v[2]),
               2 * (q[0] * v[1] - q[1] * v[0])};
  return {v[0] + q[3] * t[0] + q[1] * t[2] - q[2] * t[1],
          v[1] + q[3] * t[1] + q[2] * t[0] - q[0] * t[2],
          v[2] + q[3] * t[2] + q[0] * t[1] - q[1] * t[0]};
}

inline Quaternion yaw(float angle) {
  return {0, std::sin(angle * 0.5f), 0, std::cos(angle * 0.5f)};
}

struct Anchor {
  Vec3 origin{};
  Quaternion heading{0, 0, 0, 1};
};

inline Anchor recenter(const std::array<EyeView, 2> &eyes) {
  const Vec3 forward = rotate(eyes[0].orientation, {0, 0, -1});
  Anchor anchor;
  for (unsigned axis = 0; axis < 3; ++axis)
    anchor.origin[axis] = (eyes[0].position[axis] + eyes[1].position[axis]) * 0.5f;
  if (std::hypot(forward[0], forward[2]) > 0.01f)
    anchor.heading = yaw(std::atan2(-forward[0], -forward[2]));
  return anchor;
}

inline Matrix eyeMatrix(const Anchor &anchor, const EyeView &eye) {
  constexpr float nearPlane = 0.05f;
  constexpr float farPlane = 180.0f;
  const float xScale = 2 / (eye.right - eye.left);
  const float xOffset = (eye.right + eye.left) / (eye.right - eye.left);
  const float yScale = 2 / (eye.up - eye.down);
  const float yOffset = (eye.up + eye.down) / (eye.up - eye.down);
  const float depthScale = farPlane / (farPlane - nearPlane);
  Matrix matrix{};
  for (unsigned column = 0; column < 4; ++column) {
    Vec3 v{};
    if (column < 3) {
      v[column] = 1;
      v = rotate(anchor.heading, v);
    } else {
      for (unsigned axis = 0; axis < 3; ++axis)
        v[axis] = anchor.origin[axis] - eye.position[axis];
    }
    v = rotate(inverse(eye.orientation), v);
    const float z = -v[2];
    matrix[column * 4 + 0] = xScale * v[0] - xOffset * z;
    matrix[column * 4 + 1] = -yScale * v[1] + yOffset * z;
    matrix[column * 4 + 2] = depthScale * (z - (column == 3 ? nearPlane : 0));
    matrix[column * 4 + 3] = z;
  }
  return matrix;
}
} // namespace ps1::gpu::vr
