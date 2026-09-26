#include "clip_state.hpp"
#include <cmath>

namespace nwd::detail {
LoadedClipSet loaded_legacy_clip_set(std::span<const ClipPlane> planes,
                                     const ClipSettings &settings,
                                     uint32_t version, uint32_t declared_count,
                                     ClipLoadProfile profile) {
  if (profile != ClipLoadProfile::reader_2017 &&
      profile != ClipLoadProfile::reader_2026)
    throw Error("clip state: unknown load profile");
  if (version < 18 || version >= 116 || declared_count > 6 ||
      planes.size() != (declared_count ? declared_count : 1) ||
      settings.mode.has_value() != (version >= 107) ||
      settings.enabled.has_value() != (version >= 107) ||
      settings.box.has_value() != (version >= 107) || settings.box_orientation)
    throw Error("clip state: inconsistent legacy saved fields");
  LoadedClipSet result;
  result.profile = profile;
  result.wire_version = version;
  result.serialized_plane_count = declared_count;
  result.linked = settings.linked;
  result.current_plane = settings.current_plane;
  result.mode = settings.mode.value_or(0);
  result.bounds.range = settings.range;
  result.box_defaulted = version < 107;
  result.bounds.box = settings.box.value_or(ClipBounds{{1, 1, 1}, {0, 0, 0}});
  result.orientation_defaulted = true;
  constexpr std::array<uint32_t, 6> alignments{6, 7, 3, 2, 4, 5};
  const auto default_frame =
      loaded_legacy_clip_frame(LegacyClipPlane{false, 0, {0, 1, 0}, 0});
  for (unsigned i = 0; i < 6; ++i) {
    auto &plane = result.planes[i];
    plane.alignment = alignments[i];
    plane.frame = default_frame;
    if (i >= planes.size())
      continue;
    const auto *raw = std::get_if<LegacyClipPlane>(&planes[i].value);
    if (!raw)
      throw Error("clip state: modern plane in legacy set");
    if (planes[i].alignment == 0)
      continue;
    plane.state = raw->enabled ? 1 : 2;
    plane.alignment = planes[i].alignment;
    plane.frame = loaded_legacy_clip_frame(*raw);
  }
  if (result.mode == 1)
    result.enabled = settings.enabled.value_or(false);
  else
    for (const auto &plane : result.planes)
      if (plane.state == 1)
        result.enabled = true;
  result.box_transform =
      clip_box_transform(result.bounds.box, result.bounds.orientation);
  return result;
}

LoadedClipSet loaded_modern_clip_set(std::span<const ClipPlane> planes,
                                     const ClipSettings &settings,
                                     uint32_t version,
                                     ClipLoadProfile profile) {
  if (profile != ClipLoadProfile::reader_2017 &&
      profile != ClipLoadProfile::reader_2026)
    throw Error("clip state: unknown load profile");
  if (version < 116)
    throw Error("clip state: legacy plane loading not implemented");
  if (planes.size() != 6 || !settings.mode || !settings.enabled ||
      !settings.box || settings.box_orientation.has_value() != (version >= 118))
    throw Error("clip state: inconsistent modern saved fields");
  LoadedClipSet result;
  result.profile = profile;
  result.wire_version = version;
  result.linked = settings.linked;
  result.current_plane = settings.current_plane;
  result.mode = *settings.mode;
  result.enabled = *settings.enabled;
  for (unsigned i = 0; i < 6; ++i) {
    const auto *raw = std::get_if<ClipPlaneFrame>(&planes[i].value);
    if (!raw)
      throw Error("clip state: legacy plane in modern set");
    auto &plane = result.planes[i];
    plane.state = raw->state;
    plane.alignment = planes[i].alignment;
    plane.frame = loaded_clip_frame(*raw);
  }
  result.bounds.box = *settings.box;
  result.bounds.range = settings.range;
  // 2026 ReadContents repairs nonempty boxes with volume <=0. NaN
  // comparisons skip the repair, matching the native unordered branch; the
  // checked matrix construction below then reports nonfinite box coordinates.
  auto &box = result.bounds.box;
  if (profile == ClipLoadProfile::reader_2026 && !(box.min[0] > box.max[0])) {
    const double volume =
        ((box.max[1] - box.min[1]) * (box.max[0] - box.min[0])) *
        (box.max[2] - box.min[2]);
    if (volume <= 0) {
      box = {{1, 1, 1}, {0, 0, 0}};
      result.mode = 0;
      result.box_reset = true;
    }
  }
  result.orientation_defaulted = version < 118;
  if (settings.box_orientation)
    result.bounds.orientation = *settings.box_orientation;
  if (profile == ClipLoadProfile::reader_2026)
    for (const double x : result.bounds.orientation)
      if (std::isnan(x)) {
        result.bounds.orientation = {0, 0, 0, 1};
        result.orientation_nan_reset = true;
        break;
      }
  result.box_transform =
      clip_box_transform(result.bounds.box, result.bounds.orientation);
  return result;
}

LoadedClipSet
transformed_clip_set_general_affine(const LoadedClipSet &source,
                                    const std::array<double, 16> &matrix,
                                    uint32_t alignment_flags) {
  LoadedClipSet result = source;
  for (auto &plane : result.planes) {
    if (plane.state == 0)
      continue;
    plane.frame = transformed_surface_frame(plane.frame, matrix);
    if (alignment_flags & 7)
      plane.alignment = 10;
  }
  const auto box = transformed_clip_box(source.bounds, matrix);
  result.bounds = box.state;
  result.box_transform = box.box_transform;
  return result;
}
} // namespace nwd::detail
