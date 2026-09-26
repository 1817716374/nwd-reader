#pragma once
#include "nwd/reader.hpp"

namespace nwd::detail {
// Explicit runtime box state, separate from optional serialized ClipSettings.
// Loading defaults/repairs, plane gating and native Transform3 tags are not
// inferred here. Matrices use column vectors and column-major storage.
using ::nwd::ClipBoxState;
struct TransformedClipBox {
  ClipBoxState state;
  std::array<double, 16> box_transform;
};
// General affine bound accumulation. Native emptiness is min.x > max.x;
// empty finite bounds are copied unchanged. Zero-volume boxes are preserved.
ClipBounds affine_clip_bounds(const ClipBounds &,
                              const std::array<double, 16> &);
// Raw XYZW matrix construction around the box center; no normalization or
// empty-box special case. Checked finite arithmetic, as used after loading.
std::array<double, 16> clip_box_transform(const ClipBounds &,
                                          const std::array<double, 4> &);
// Applies the checked general-matrix Factor path: box by stretch+translation,
// orientation by normalized (factor rotation * old orientation), range by the
// full affine. The resulting box transform rotates about the new box center.
// Throws Error for invalid inputs, factor failure or nonfinite arithmetic.
// Does not perform document placement or mutate the serialized source.
TransformedClipBox transformed_clip_box(const ClipBoxState &,
                                        const std::array<double, 16> &);
} // namespace nwd::detail
