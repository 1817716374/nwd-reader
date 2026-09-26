#include "surface_frame.hpp"
#include <cmath>

namespace nwd::detail {
namespace {
using Vector = std::array<double, 3>;
void finite(const Vector &v) {
  for (double x : v)
    if (!std::isfinite(x))
      throw Error("surface frame: nonfinite coordinate");
}
Vector cross(const Vector &a, const Vector &b) {
  Vector result{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                a[0] * b[1] - a[1] * b[0]};
  finite(result);
  return result;
}
double dot(const Vector &a, const Vector &b) {
  return (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2];
}
Vector unit(Vector v) {
  finite(v);
  // Native UnitVec uses a direct sum of squares and an exact zero branch.
  // In particular, an underflowed squared length produces zero, not invented
  // axes; overflow/nonfinite arithmetic is rejected by this checked helper.
  const double length = std::sqrt(dot(v, v));
  if (!std::isfinite(length))
    throw Error("surface frame: direction length overflow");
  if (length == 0)
    return {};
  for (double &x : v)
    x /= length;
  return v;
}
Vector linear(const std::array<double, 16> &m, const Vector &v) {
  Vector out{};
  for (unsigned i = 0; i < 3; ++i)
    out[i] = (m[i] * v[0] + m[4 + i] * v[1]) + m[8 + i] * v[2];
  finite(out);
  return out;
}
} // namespace

SurfaceFrame loaded_clip_frame(const ClipPlaneFrame &raw) {
  finite(raw.location);
  SurfaceFrame out;
  out.location = raw.location;
  out.z = unit(raw.direction);
  const auto requested_x = unit(raw.x_direction);
  // SetXDirection projects the requested X perpendicular to the loaded Z.
  out.x = unit(cross(out.z, cross(requested_x, out.z)));
  out.y = unit(cross(out.z, out.x));
  return out;
}

SurfaceFrame loaded_legacy_clip_frame(const LegacyClipPlane &raw) {
  finite(raw.normal);
  if (!std::isfinite(raw.distance))
    throw Error("surface frame: nonfinite legacy plane distance");
  SurfaceFrame out;
  out.z = raw.normal;
  for (unsigned i = 0; i < 3; ++i)
    out.location[i] = raw.distance * raw.normal[i];
  finite(out.location);
  const auto &n = raw.normal;
  const double ax = std::abs(n[0]), ay = std::abs(n[1]), az = std::abs(n[2]);
  // Native SetValue(position, UnitVec) copies Z without normalizing. The
  // smallest-axis comparisons and ties determine X's sign and ordering.
  if (ax >= ay && az >= ay)
    out.x = ax > az ? Vector{-n[2], 0, n[0]} : Vector{n[2], 0, -n[0]};
  else if (ay >= ax && az >= ax)
    out.x = ay > az ? Vector{0, -n[2], n[1]} : Vector{0, n[2], -n[1]};
  else
    out.x = ax > ay ? Vector{-n[1], n[0], 0} : Vector{n[1], -n[0], 0};
  out.x = unit(out.x);
  out.y = unit(cross(out.z, out.x));
  return out;
}

SurfaceFrame transformed_surface_frame(const SurfaceFrame &source,
                                       const std::array<double, 16> &m) {
  for (double x : m)
    if (!std::isfinite(x))
      throw Error("surface frame: nonfinite affine matrix");
  if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1)
    throw Error("surface frame: non-affine matrix");
  finite(source.location);
  finite(source.x);
  finite(source.y);
  finite(source.z);
  SurfaceFrame out;
  out.location = linear(m, source.location);
  for (unsigned i = 0; i < 3; ++i)
    out.location[i] += m[12 + i];
  finite(out.location);
  out.x = unit(linear(m, source.x));
  const auto transformed_y = unit(linear(m, source.y));
  const auto transformed_z = unit(linear(m, source.z));
  const auto product = cross(out.x, transformed_y);
  out.z = unit(product);
  out.y = unit(cross(out.z, out.x));
  if (dot(product, transformed_z) < 0)
    for (double &x : out.z)
      x = -x;
  return out;
}
} // namespace nwd::detail
