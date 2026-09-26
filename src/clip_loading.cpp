#include "clip_state.hpp"

namespace nwd {
std::optional<LoadedClipSet>
loaded_clip_set(std::span<const ViewFields> planes, const ViewFields &settings,
                std::optional<uint32_t> declared_count, uint32_t version,
                ClipLoadProfile profile) {
  if (profile != ClipLoadProfile::reader_2017 &&
      profile != ClipLoadProfile::reader_2026)
    throw Error("clip load: unknown profile");
  if (planes.empty() && settings.integers.empty() && settings.numbers.empty() &&
      settings.strings.empty() && !declared_count)
    return std::nullopt;
  if (version < 18)
    throw Error("clip load: unsupported version before18");
  if (!declared_count || *declared_count > 6 ||
      (version >= 116
           ? *declared_count != 6 || planes.size() != 6
           : planes.size() != (*declared_count ? *declared_count : 1)))
    throw Error("clip load: missing or inconsistent declared plane count");
  std::array<ClipPlane, 6> named;
  for (size_t i = 0; i < planes.size(); ++i)
    named[i] = clip_plane_fields(planes[i], version);
  const auto parsed = clip_settings_fields(settings, version);
  const std::span<const ClipPlane> present(named.data(), planes.size());
  if (version >= 116)
    return detail::loaded_modern_clip_set(present, parsed, version, profile);
  return detail::loaded_legacy_clip_set(present, parsed, version,
                                        *declared_count, profile);
}

std::optional<LoadedClipSet>
CurrentView::loaded_clipping(ClipLoadProfile profile) const {
  return loaded_clip_set(clip_planes, clip_set, clip_declared_plane_count,
                         wire_version, profile);
}

std::optional<LoadedClipSet>
AnimationKeyFrame::loaded_clipping(uint32_t version,
                                   ClipLoadProfile profile) const {
  return loaded_clip_set(clip_planes, clip_set, clip_declared_plane_count,
                         version, profile);
}

ActiveClipPlanes active_clip_planes(const LoadedClipSet &source) {
  return detail::query_active_clip_planes(source);
}
} // namespace nwd
