#pragma once
#include <array>
#include <optional>

namespace nwd::detail {
enum class AffineFactorStatus {
  ok,
  nonfinite,
  not_affine,
  singular_or_ill_conditioned,
  no_convergence,
  numerical_failure
};

// Internal checked general-matrix path, not an emulation of tagged native
// Transform3 fast paths or their handling of degenerate values. All matrices
// use column-major storage and multiply column vectors.
struct AffineFactors {
  std::array<double, 9> rotation{}, stretch{}, scale_axes{};
  std::array<double, 3> scales{}, translation{};
  // A = rotation * stretch; stretch = scale_axes * diag(scales) * scale_axes^T.
  // Both rotations are proper. Every scale has the sign of det(A); scales are
  // not sorted. Eigenvectors in repeated eigenspaces are not unique.
  double relative_reconstruction_error = 0, orthogonality_error = 0;
};
struct AffineFactorResult {
  AffineFactorStatus status = AffineFactorStatus::numerical_failure;
  std::optional<AffineFactors> value;
};

// No heap allocation. Rejects non-affine/nonfinite inputs and matrices whose
// smallest squared principal scale is <= 64*epsilon times the largest.
// Positive rescaling before the Gram matrix avoids overflow/underflow; this
// differs intentionally from native floating-point exception/fallback behavior.
AffineFactorResult factor_affine(const std::array<double, 16> &) noexcept;
} // namespace nwd::detail
