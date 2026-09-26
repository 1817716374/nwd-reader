#pragma once
#include "clip_box.hpp"
#include "surface_frame.hpp"

namespace nwd::detail {
using ::nwd::active_clip_planes;
using ::nwd::ActiveClipPlanes;
using ::nwd::ClipLoadProfile;
using ::nwd::ClipPlaneEquation;
using ::nwd::LoadedClipPlane;
using ::nwd::LoadedClipSet;
// Fresh loading of version >=116 with six serialized modern frames. Unknown
// state/alignment/mode values and current_plane are preserved. Checked helpers
// reject nonfinite geometry after the profile's observed load-time repairs.
LoadedClipSet loaded_modern_clip_set(std::span<const ClipPlane>,
                                     const ClipSettings &,
                                     uint32_t wire_version, ClipLoadProfile);
// Fresh legacy >=18,<116 load. Count0 still includes the independently saved
// first plane. Unsaved planes keep constructor defaults; alignment0 only resets
// state and leaves the default frame/alignment. Both observed profiles share
// this branch, including its lack of modern2026 box repairs.
LoadedClipSet loaded_legacy_clip_set(std::span<const ClipPlane>,
                                     const ClipSettings &,
                                     uint32_t wire_version,
                                     uint32_t declared_count, ClipLoadProfile);
// General affine path. Only plane alignment uses explicit native classification
// flags (low three bits); these flags do not select native Factor fast paths.
// State0 planes are copied unchanged, every other state propagates. Enabled,
// mode, linked and current_plane do not gate this operation. No world
// placement.
LoadedClipSet
transformed_clip_set_general_affine(const LoadedClipSet &,
                                    const std::array<double, 16> &,
                                    uint32_t alignment_transform_flags);
// Disabled/unknown-mode sets and empty boxes yield no active equations. Plane
// mode includes exactly state1 and preserves the frame Z length. Box mode uses
// its cached rotation-tagged box matrix, including native nonunit-q behavior.
ActiveClipPlanes query_active_clip_planes(const LoadedClipSet &);
// Native ConvertToPlanes behavior: converts axial box bounds, without applying
// the box orientation. Existing frame handedness is retained. Mode0 is a no-op.
// Empty boxes clear mode/states/linked/index, retaining geometry and enabled.
LoadedClipSet clip_set_converted_to_planes(const LoadedClipSet &);
} // namespace nwd::detail
