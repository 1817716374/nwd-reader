#pragma once
#include <array>

namespace nwd::detail {
// XYZW to column-major matrix, using the stored components without normalizing.
std::array<double, 9> quaternion_matrix(const std::array<double, 4> &) noexcept;
// Input must be a checked proper rotation. Selects the largest quaternion
// component; diagonal ties prefer Z, then Y, then X. No sign canonicalization
// or renormalization is applied after conversion.
std::array<double, 4>
rotation_quaternion(const std::array<double, 9> &) noexcept;
} // namespace nwd::detail
