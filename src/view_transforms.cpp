#include "nwd/reader.hpp"
#include "affine_factor.hpp"
#include "rotation.hpp"
#include <cmath>

namespace nwd::detail {
std::array<double, 9>
quaternion_matrix(const std::array<double, 4> &q) noexcept {
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  const double xx = (x + x) * x, yy = (y + y) * y, zz = (z + z) * z;
  const double xy = (x + x) * y, xz = (x + x) * z, yz = (y + y) * z;
  const double xw = (x + x) * w, yw = (y + y) * w, zw = (z + z) * w;
  return {1 - yy - zz, xy + zw, xz - yw, xy - zw,    1 - zz - xx,
          yz + xw,     xz + yw, yz - xw, 1 - xx - yy};
}

std::array<double, 4>
rotation_quaternion(const std::array<double, 9> &m) noexcept {
  unsigned i = m[0] > m[4] ? 0 : 1;
  if (!(m[4 * i] > m[8]))
    i = 2;
  const double trace = (m[0] + m[4]) + m[8];
  std::array<double, 4> q{};
  if (trace > m[4 * i]) {
    q[3] = .5 * std::sqrt(trace + 1);
    const double inverse = 1 / (4 * q[3]);
    q[0] = (m[5] - m[7]) * inverse;
    q[1] = (m[6] - m[2]) * inverse;
    q[2] = (m[1] - m[3]) * inverse;
  } else {
    const unsigned j = (i + 1) % 3, k = (j + 1) % 3;
    q[i] = .5 * std::sqrt(((m[4 * i] - m[4 * j]) - m[4 * k]) + 1);
    const double inverse = 1 / (4 * q[i]);
    q[j] = (m[3 * i + j] + m[3 * j + i]) * inverse;
    q[k] = (m[3 * i + k] + m[3 * k + i]) * inverse;
    q[3] = (m[3 * j + k] - m[3 * k + j]) * inverse;
  }
  return q;
}
} // namespace nwd::detail

namespace nwd {
Camera transformed_camera(const Camera &camera,
                          const std::array<double, 16> &matrix) {
  if (camera.projection > 1)
    throw Error("camera transform: invalid projection");
  for (double x : matrix)
    if (!std::isfinite(x))
      throw Error("camera transform: nonfinite matrix");
  if (matrix[3] != 0 || matrix[7] != 0 || matrix[11] != 0 || matrix[15] != 1)
    throw Error("camera transform: non-affine matrix");
  for (double x : camera.orientation)
    if (!std::isfinite(x))
      throw Error("camera transform: nonfinite orientation");
  for (double x : camera.position)
    if (!std::isfinite(x))
      throw Error("camera transform: nonfinite position");

  const auto pose = detail::quaternion_matrix(camera.orientation);
  std::array<double, 16> combined{};
  combined[15] = 1;
  for (unsigned c = 0; c < 3; ++c)
    for (unsigned r = 0; r < 3; ++r)
      combined[c * 4 + r] =
          (matrix[r] * pose[c * 3] + matrix[4 + r] * pose[c * 3 + 1]) +
          matrix[8 + r] * pose[c * 3 + 2];
  for (unsigned r = 0; r < 3; ++r)
    combined[12 + r] =
        ((matrix[r] * camera.position[0] + matrix[4 + r] * camera.position[1]) +
         matrix[8 + r] * camera.position[2]) +
        matrix[12 + r];
  const auto factor = detail::factor_affine(combined);
  switch (factor.status) {
  case detail::AffineFactorStatus::ok:
    break;
  case detail::AffineFactorStatus::singular_or_ill_conditioned:
    throw Error("camera transform: singular or ill-conditioned pose");
  case detail::AffineFactorStatus::no_convergence:
    throw Error("camera transform: pose decomposition did not converge");
  default:
    throw Error("camera transform: numerically unreliable pose");
  }
  const auto &f = *factor.value;
  Camera result = camera;
  result.position = f.translation;
  result.orientation = detail::rotation_quaternion(f.rotation);
  for (double x : result.orientation)
    if (!std::isfinite(x))
      throw Error("camera transform: nonfinite decomposed orientation");
  if (camera.projection == 1) {
    const double scale =
        std::abs((f.scales[1] + f.scales[0]) + f.scales[2]) / 3;
    if (!std::isfinite(scale))
      throw Error("camera transform: distance scale overflow");
    for (unsigned i = 1; i <= 3; ++i) {
      result.parameters[i] *= scale;
      if (!std::isfinite(result.parameters[i]))
        throw Error("camera transform: nonfinite transformed distance");
    }
  }
  return result;
}
} // namespace nwd
