#include "affine_factor.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace nwd::detail {
namespace {
using M3 = std::array<double, 9>;
constexpr M3 identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
double &at(M3 &m, unsigned row, unsigned column) { return m[column * 3 + row]; }
double at(const M3 &m, unsigned row, unsigned column) {
  return m[column * 3 + row];
}
M3 multiply(const M3 &a, const M3 &b) {
  M3 result{};
  for (unsigned c = 0; c < 3; ++c)
    for (unsigned r = 0; r < 3; ++r)
      for (unsigned k = 0; k < 3; ++k)
        at(result, r, c) += at(a, r, k) * at(b, k, c);
  return result;
}
M3 transpose(const M3 &a) {
  M3 result{};
  for (unsigned c = 0; c < 3; ++c)
    for (unsigned r = 0; r < 3; ++r)
      at(result, r, c) = at(a, c, r);
  return result;
}
double determinant(const M3 &a) {
  return a[0] * (a[4] * a[8] - a[7] * a[5]) -
         a[3] * (a[1] * a[8] - a[7] * a[2]) +
         a[6] * (a[1] * a[5] - a[4] * a[2]);
}
double difference(const M3 &a, const M3 &b) {
  double result = 0;
  for (unsigned i = 0; i < 9; ++i)
    result = std::max(result, std::abs(a[i] - b[i]));
  return result;
}
bool finite(const M3 &a) {
  return std::all_of(a.begin(), a.end(),
                     [](double x) { return std::isfinite(x); });
}

// Cyclic symmetric Jacobi rotations, with a fixed sweep limit. Unlike the
// native implementation, convergence is relative and checked by residuals.
bool diagonalize(M3 &a, M3 &axes) {
  constexpr double eps = std::numeric_limits<double>::epsilon();
  for (unsigned sweep = 0; sweep <= 50; ++sweep) {
    const double diagonal =
        std::max({std::abs(a[0]), std::abs(a[4]), std::abs(a[8])});
    if (std::max({std::abs(a[3]), std::abs(a[6]), std::abs(a[7])}) <=
        eps * diagonal)
      return true;
    if (sweep == 50)
      return false;
    for (unsigned p = 0; p < 2; ++p)
      for (unsigned q = p + 1; q < 3; ++q) {
        const double off = at(a, p, q);
        if (off == 0)
          continue;
        const double delta = (at(a, q, q) - at(a, p, p)) * 0.5;
        // Bounded tangent; copysign chooses a deterministic rotation for equal
        // diagonal entries without dividing by a potentially tiny off-diagonal.
        const double tangent =
            off / (delta + std::copysign(std::hypot(delta, off), delta));
        const double cosine = 1 / std::hypot(1.0, tangent);
        const double sine = tangent * cosine;
        at(a, p, p) -= tangent * off;
        at(a, q, q) += tangent * off;
        at(a, p, q) = at(a, q, p) = 0;
        for (unsigned k = 0; k < 3; ++k) {
          if (k != p && k != q) {
            const double x = at(a, k, p), y = at(a, k, q);
            at(a, k, p) = at(a, p, k) = cosine * x - sine * y;
            at(a, k, q) = at(a, q, k) = sine * x + cosine * y;
          }
          const double x = at(axes, k, p), y = at(axes, k, q);
          at(axes, k, p) = cosine * x - sine * y;
          at(axes, k, q) = sine * x + cosine * y;
        }
      }
  }
  return false;
}
} // namespace

AffineFactorResult
factor_affine(const std::array<double, 16> &matrix) noexcept {
  const auto fail = [](AffineFactorStatus status) {
    return AffineFactorResult{status, {}};
  };
  for (double x : matrix)
    if (!std::isfinite(x))
      return fail(AffineFactorStatus::nonfinite);
  if (matrix[3] != 0 || matrix[7] != 0 || matrix[11] != 0 || matrix[15] != 1)
    return fail(AffineFactorStatus::not_affine);
  M3 a{};
  double magnitude = 0;
  for (unsigned c = 0; c < 3; ++c)
    for (unsigned r = 0; r < 3; ++r) {
      at(a, r, c) = matrix[c * 4 + r];
      magnitude = std::max(magnitude, std::abs(at(a, r, c)));
    }
  if (magnitude == 0)
    return fail(AffineFactorStatus::singular_or_ill_conditioned);
  for (double &x : a)
    x /= magnitude;
  const double det = determinant(a);
  if (det == 0)
    return fail(AffineFactorStatus::singular_or_ill_conditioned);

  M3 gram = multiply(transpose(a), a), axes = identity;
  if (!diagonalize(gram, axes))
    return fail(AffineFactorStatus::no_convergence);
  const double maximum = std::max({gram[0], gram[4], gram[8]});
  const double minimum = std::min({gram[0], gram[4], gram[8]});
  if (!(minimum > maximum * (64 * std::numeric_limits<double>::epsilon())))
    return fail(AffineFactorStatus::singular_or_ill_conditioned);

  M3 stretch{}, inverse_stretch{};
  AffineFactors result;
  result.scale_axes = axes;
  const double sign = det < 0 ? -1 : 1;
  for (unsigned k = 0; k < 3; ++k) {
    const double scale = sign * std::sqrt(at(gram, k, k));
    result.scales[k] = scale * magnitude;
    if (!std::isfinite(result.scales[k]) || result.scales[k] == 0)
      return fail(AffineFactorStatus::numerical_failure);
    for (unsigned c = 0; c < 3; ++c)
      for (unsigned r = 0; r < 3; ++r) {
        const double product = at(axes, r, k) * at(axes, c, k);
        at(stretch, r, c) += product * scale;
        at(inverse_stretch, r, c) += product / scale;
      }
  }
  result.rotation = multiply(a, inverse_stretch);
  result.relative_reconstruction_error =
      difference(multiply(result.rotation, stretch), a);
  result.orthogonality_error = difference(
      multiply(transpose(result.rotation), result.rotation), identity);
  // Residuals are measured before undoing the common positive scale. Reject
  // unstable outputs instead of silently orthogonalizing or inventing axes.
  if (!finite(result.rotation) || !finite(stretch) ||
      !std::isfinite(result.relative_reconstruction_error) ||
      !std::isfinite(result.orthogonality_error) ||
      result.relative_reconstruction_error > 2e-10 ||
      result.orthogonality_error > 2e-10 ||
      std::abs(determinant(result.rotation) - 1) > 2e-10)
    return fail(AffineFactorStatus::numerical_failure);
  result.stretch = stretch;
  for (double &x : result.stretch)
    x *= magnitude;
  if (!finite(result.stretch))
    return fail(AffineFactorStatus::numerical_failure);
  // Subnormal output components may lose appreciable precision even though
  // the normalized calculation passed. Check the actual returned components.
  M3 returned_stretch{};
  for (unsigned i = 0; i < 9; ++i)
    returned_stretch[i] = result.stretch[i] / magnitude;
  result.relative_reconstruction_error =
      difference(multiply(result.rotation, returned_stretch), a);
  if (result.relative_reconstruction_error > 2e-10)
    return fail(AffineFactorStatus::numerical_failure);
  M3 returned_spectral{};
  for (unsigned k = 0; k < 3; ++k)
    for (unsigned c = 0; c < 3; ++c)
      for (unsigned r = 0; r < 3; ++r)
        at(returned_spectral, r, c) +=
            at(axes, r, k) * at(axes, c, k) * (result.scales[k] / magnitude);
  if (difference(returned_spectral, returned_stretch) > 2e-10)
    return fail(AffineFactorStatus::numerical_failure);
  std::copy_n(matrix.begin() + 12, 3, result.translation.begin());
  return {AffineFactorStatus::ok, result};
}
} // namespace nwd::detail
