#include "clip_state.hpp"
#include <cmath>

namespace nwd::detail {
namespace {
using Vector = std::array<double, 3>;
constexpr std::array<Vector, 6> normals{
    {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {-1, 0, 0}}};
constexpr std::array<uint32_t, 6> alignments{6, 7, 3, 2, 4, 5};
constexpr std::array<bool, 6> uses_max{true, false, true, false, false, true};
template <size_t N> void finite(const std::array<double, N> &v) {
  for (double x : v)
    if (!std::isfinite(x))
      throw Error("clip planes: nonfinite coordinate");
}
double dot(const Vector &a, const Vector &b) {
  const double out = (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2];
  if (!std::isfinite(out))
    throw Error("clip planes: dot product overflow");
  return out;
}
bool is_direct(const SurfaceFrame &f) {
  finite(f.x);
  finite(f.y);
  finite(f.z);
  const Vector xy{f.x[1] * f.y[2] - f.x[2] * f.y[1],
                  f.x[2] * f.y[0] - f.x[0] * f.y[2],
                  f.x[0] * f.y[1] - f.x[1] * f.y[0]};
  finite(xy);
  return dot(xy, f.z) > 0;
}
} // namespace

LoadedClipSet clip_set_converted_to_planes(const LoadedClipSet &source) {
  auto out = source;
  if (out.mode == 0)
    return out;
  const auto &box = out.bounds.box;
  out.mode = 0;
  if (box.min[0] > box.max[0]) {
    for (auto &p : out.planes)
      p.state = 0;
    out.linked = false;
    out.current_plane = 0;
    return out;
  }
  finite(box.min);
  finite(box.max);
  for (unsigned i = 0; i < 6; ++i) {
    auto &p = out.planes[i];
    const bool direct = is_direct(p.frame);
    p.frame = loaded_legacy_clip_frame({false, 0, normals[i], 0});
    p.frame.location = uses_max[i] ? box.max : box.min;
    if (!direct)
      for (double &x : p.frame.y)
        x = -x;
    p.state = out.enabled ? 1 : 2;
    p.alignment = alignments[i];
  }
  return out;
}

ActiveClipPlanes query_active_clip_planes(const LoadedClipSet &source) {
  ActiveClipPlanes out;
  if (!source.enabled)
    return out;
  if (source.mode == 0) {
    for (unsigned i = 0; i < 6; ++i) {
      const auto &p = source.planes[i];
      if (p.state != 1)
        continue;
      finite(p.frame.location);
      finite(p.frame.z);
      out.planes[out.count++] = {p.frame.z, dot(p.frame.z, p.frame.location),
                                 i};
    }
    return out;
  }
  if (source.mode != 1)
    return out;
  const auto &box = source.bounds.box;
  if (box.min[0] > box.max[0])
    return out;
  finite(box.min);
  finite(box.max);
  const auto &m = source.box_transform;
  finite(m);
  if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1)
    throw Error("clip planes: non-affine box matrix");
  const Vector t{m[12], m[13], m[14]};
  for (unsigned i = 0; i < 6; ++i) {
    const auto &n = normals[i];
    const double d = dot(n, uses_max[i] ? box.max : box.min);
    Vector transformed;
    for (unsigned row = 0; row < 3; ++row)
      transformed[row] =
          (m[row] * n[0] + m[row + 4] * n[1]) + m[row + 8] * n[2];
    finite(transformed);
    // ComputeBoxTransform always carries rotation+translation tags. Native
    // Inverse therefore transposes its linear part, even for a nonunit saved
    // quaternion. Plane's affine-inverse path then normalizes all coefficients.
    const double length = std::sqrt(dot(transformed, transformed));
    if (length == 0)
      throw Error("clip planes: collapsed box normal");
    const double inverse = 1 / length;
    const double distance = (d + dot(transformed, t)) * inverse;
    if (!std::isfinite(distance))
      throw Error("clip planes: distance overflow");
    for (double &x : transformed)
      x *= inverse;
    finite(transformed);
    out.planes[out.count++] = {transformed, distance, i};
  }
  return out;
}
} // namespace nwd::detail
