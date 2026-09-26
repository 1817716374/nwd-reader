#include "clip_box.hpp"
#include "affine_factor.hpp"
#include "rotation.hpp"
#include <algorithm>
#include <cmath>

namespace nwd::detail {
namespace {
template <size_t N> void finite(const std::array<double, N> &v) {
  for (const double x : v)
    if (!std::isfinite(x))
      throw Error("clip box: nonfinite value or arithmetic overflow");
}
void affine(const std::array<double, 16> &m) {
  finite(m);
  if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1)
    throw Error("clip box: non-affine matrix");
}
std::array<double, 4> compose(const std::array<double, 4> &old,
                              const std::array<double, 4> &rotation) {
  finite(old);
  // Native old *= rotation uses Hamilton rotation * old (column convention)
  // and then normalizes. Unlike camera construction, this is NOT a raw copy.
  std::array<double, 4> q{
      ((rotation[0] * old[3] + old[0] * rotation[3]) + old[2] * rotation[1]) -
          old[1] * rotation[2],
      ((rotation[1] * old[3] + old[1] * rotation[3]) + old[0] * rotation[2]) -
          old[2] * rotation[0],
      ((old[2] * rotation[3] + old[3] * rotation[2]) + old[1] * rotation[0]) -
          old[0] * rotation[1],
      ((old[3] * rotation[3] - rotation[0] * old[0]) - rotation[1] * old[1]) -
          old[2] * rotation[2]};
  finite(q);
  const double length =
      std::sqrt(((q[0] * q[0] + q[1] * q[1]) + q[2] * q[2]) + q[3] * q[3]);
  // Native divides by zero for a zero/underflowed quaternion; do not invent an
  // identity fallback or silently propagate NaNs in this checked operation.
  if (!std::isfinite(length) || length == 0)
    throw Error("clip box: invalid orientation normalization");
  const double inverse = 1 / length;
  for (double &x : q)
    x *= inverse;
  finite(q);
  return q;
}
} // namespace

ClipBounds affine_clip_bounds(const ClipBounds &source,
                              const std::array<double, 16> &m) {
  affine(m);
  finite(source.min);
  finite(source.max);
  if (source.min[0] > source.max[0])
    return source;
  ClipBounds result;
  for (unsigned row = 0; row < 3; ++row) {
    result.min[row] = result.max[row] = m[12 + row];
    for (unsigned col = 0; col < 3; ++col) {
      const double a = m[col * 4 + row] * source.min[col];
      const double b = m[col * 4 + row] * source.max[col];
      result.min[row] += std::min(a, b);
      result.max[row] += std::max(a, b);
    }
  }
  finite(result.min);
  finite(result.max);
  return result;
}

TransformedClipBox transformed_clip_box(const ClipBoxState &source,
                                        const std::array<double, 16> &m) {
  const auto factors = factor_affine(m);
  if (!factors.value)
    throw Error("clip box: affine factorization failed");
  const auto &f = *factors.value;
  std::array<double, 16> stretch{0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 1};
  for (unsigned col = 0; col < 3; ++col)
    for (unsigned row = 0; row < 3; ++row)
      stretch[col * 4 + row] = f.stretch[col * 3 + row];
  for (unsigned row = 0; row < 3; ++row)
    stretch[12 + row] = f.translation[row];
  TransformedClipBox result;
  result.state.box = affine_clip_bounds(source.box, stretch);
  result.state.orientation =
      compose(source.orientation, rotation_quaternion(f.rotation));
  result.state.range = affine_clip_bounds(source.range, m);
  result.box_transform =
      clip_box_transform(result.state.box, result.state.orientation);
  return result;
}

std::array<double, 16>
clip_box_transform(const ClipBounds &box,
                   const std::array<double, 4> &orientation) {
  finite(box.min);
  finite(box.max);
  finite(orientation);
  const auto rotation = quaternion_matrix(orientation);
  finite(rotation);
  std::array<double, 3> center;
  for (unsigned row = 0; row < 3; ++row)
    center[row] = (box.min[row] + box.max[row]) * .5;
  finite(center);
  std::array<double, 16> transform{};
  transform[15] = 1;
  for (unsigned row = 0; row < 3; ++row) {
    double rotated = 0;
    for (unsigned col = 0; col < 3; ++col) {
      transform[col * 4 + row] = rotation[col * 3 + row];
      rotated += rotation[col * 3 + row] * center[col];
    }
    transform[12 + row] = center[row] - rotated;
  }
  finite(transform);
  return transform;
}
} // namespace nwd::detail
