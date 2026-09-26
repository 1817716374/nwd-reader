#pragma once
#include "nwd/reader.hpp"

namespace nwd::detail {
// Loaded frame state; all three axes are retained because mirrored frames can
// be left-handed. This is not the three-vector serialized ClipPlaneFrame.
using ::nwd::SurfaceFrame;
// Fresh modern clip plane loading (wire version >=116). Preserves zero axes
// produced by zero/parallel saved directions; throws for nonfinite arithmetic.
SurfaceFrame loaded_clip_frame(const ClipPlaneFrame &);
// Fresh legacy plane load assumes stored normal is already a UnitVec. Retains
// its raw length; origin is distance*normal, X/Y follow native axis selection.
// Caller handles alignment0, which skips geometry loading altogether.
SurfaceFrame loaded_legacy_clip_frame(const LegacyClipPlane &);
// Affine frame propagation: preserves transformed X, reconstructs Y/Z and the
// handedness implied by transformed source axes. Does not change clip state,
// alignment, box settings, or infer a document/world placement.
SurfaceFrame transformed_surface_frame(const SurfaceFrame &,
                                       const std::array<double, 16> &);
} // namespace nwd::detail
