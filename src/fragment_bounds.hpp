#pragma once

#include <nwd/reader.hpp>

#include <array>
#include <cmath>
#include <string>

namespace nwd::detail {

struct FragmentAffine {
  double linear[3][3]{};
  double translation[3]{};
};

// Reproduce the affine matrix consumed by Manage 2026's fragment-box
// transform vtable. Type 17 uses the nine column-major linear entries at
// serialized slots 0..2, 4..6 and 8..10, followed by its double translation
// at slots 13..15. The product's conversion helper ignores slots 3, 7, 11 and
// 12 when constructing LcLAffine3f.
inline bool make_fragment_affine(const Transform &transform, FragmentAffine &out,
                                 std::string &diagnostic) {
  const auto &v = transform.values;
  if (transform.type == 14) {
    for (unsigned i = 0; i < 3; ++i) {
      out.linear[i][i] = 1;
      out.translation[i] = v[i];
      if (!std::isfinite(v[i])) {
        diagnostic = "non-finite RunTransform translation";
        return false;
      }
    }
    return true;
  }
  if (transform.type == 17) {
    out.linear[0][0] = v[0];
    out.linear[1][0] = v[1];
    out.linear[2][0] = v[2];
    out.linear[0][1] = v[4];
    out.linear[1][1] = v[5];
    out.linear[2][1] = v[6];
    out.linear[0][2] = v[8];
    out.linear[1][2] = v[9];
    out.linear[2][2] = v[10];
    for (unsigned i = 0; i < 3; ++i)
      out.translation[i] = v[13 + i];
    for (const auto &row : out.linear)
      for (double x : row)
        if (!std::isfinite(x)) {
          diagnostic = "non-finite RunTransform matrix";
          return false;
        }
    for (double x : out.translation)
      if (!std::isfinite(x)) {
        diagnostic = "non-finite RunTransform translation";
        return false;
      }
    return true;
  }
  if (transform.type != 15 && transform.type != 16) {
    diagnostic = "unsupported native RunTransform type " +
                 std::to_string(transform.type);
    return false;
  }
  const double x = v[3], y = v[4], z = v[5], w = v[6];
  const double norm = x * x + y * y + z * z + w * w;
  if (!std::isfinite(norm) || std::abs(norm - 1.0) >= 1e-5) {
    diagnostic = "non-unit RunTransform rotation cannot be bounded exactly";
    return false;
  }
  const double scale = transform.type == 16 ? v[7] : 1.0;
  if (!std::isfinite(scale)) {
    diagnostic = "non-finite RunTransform scale";
    return false;
  }
  out.linear[0][0] = (1 - 2 * (y * y + z * z)) * scale;
  out.linear[0][1] = 2 * (x * y - z * w) * scale;
  out.linear[0][2] = 2 * (x * z + y * w) * scale;
  out.linear[1][0] = 2 * (x * y + z * w) * scale;
  out.linear[1][1] = (1 - 2 * (x * x + z * z)) * scale;
  out.linear[1][2] = 2 * (y * z - x * w) * scale;
  out.linear[2][0] = 2 * (x * z - y * w) * scale;
  out.linear[2][1] = 2 * (y * z + x * w) * scale;
  out.linear[2][2] = (1 - 2 * (x * x + y * y)) * scale;
  for (unsigned i = 0; i < 3; ++i) {
    out.translation[i] = v[i];
    if (!std::isfinite(out.translation[i])) {
      diagnostic = "non-finite RunTransform translation";
      return false;
    }
  }
  return true;
}

} // namespace nwd::detail
