#include "internal.hpp"
namespace nwd {
namespace {
uint32_t viewpoint_tool(uint32_t raw, uint32_t version) {
  detail::require(raw <= (version < 120 ? 12u : 699u), "viewpoint tool enum");
  return version < 120 ? (raw == 6 ? 0 : raw + 600) : raw;
}
} // namespace
ClipPlane clip_plane_fields(const ViewFields &raw, uint32_t version) {
  using detail::require;
  require(raw.integers.size() == 2 && raw.strings.empty() &&
              raw.numbers.size() == (version >= 116 ? 9u : 5u),
          "inconsistent clip plane fields");
  ClipPlane result;
  result.alignment = raw.integers[1];
  if (version < 116) {
    require(raw.integers[0] <= 1, "legacy clip plane boolean");
    auto &plane = result.value.emplace<LegacyClipPlane>();
    plane.enabled = raw.integers[0] != 0;
    plane.distance = raw.numbers[0];
    std::copy_n(raw.numbers.begin() + 1, 3, plane.normal.begin());
    plane.legacy_value = raw.numbers[4];
  } else {
    auto &plane = result.value.emplace<ClipPlaneFrame>();
    plane.state = raw.integers[0];
    std::copy_n(raw.numbers.begin(), 3, plane.location.begin());
    std::copy_n(raw.numbers.begin() + 3, 3, plane.direction.begin());
    std::copy_n(raw.numbers.begin() + 6, 3, plane.x_direction.begin());
  }
  return result;
}
ClipSettings clip_settings_fields(const ViewFields &raw, uint32_t version) {
  using detail::require;
  if (version < 18)
    throw detail::UnsupportedLayout(
        "legacy clip range layout before version18");
  require(raw.integers.size() == (version >= 107 ? 4u : 2u) &&
              raw.numbers.size() == (version >= 118   ? 16u
                                     : version >= 107 ? 12u
                                                      : 6u) &&
              raw.strings.empty(),
          "inconsistent clip set fields");
  auto boolean = [&](size_t i) {
    require(raw.integers[i] <= 1, "clip set boolean");
    return raw.integers[i] != 0;
  };
  auto bounds = [&](size_t offset) {
    ClipBounds box;
    std::copy_n(raw.numbers.begin() + offset, 3, box.min.begin());
    std::copy_n(raw.numbers.begin() + offset + 3, 3, box.max.begin());
    return box;
  };
  ClipSettings result;
  result.linked = boolean(0);
  result.current_plane = std::bit_cast<int32_t>(raw.integers[1]);
  result.range = bounds(0);
  if (version >= 107) {
    result.mode = raw.integers[2];
    result.enabled = boolean(3);
    result.box = bounds(6);
  }
  if (version >= 118) {
    std::array<double, 4> rotation;
    std::copy_n(raw.numbers.begin() + 12, 4, rotation.begin());
    result.box_orientation = rotation;
  }
  return result;
}
ClipPlane CurrentView::named_clip_plane(size_t index) const {
  detail::require(index < clip_planes.size(), "clip plane index outside set");
  return clip_plane_fields(clip_planes[index], wire_version);
}
std::optional<ClipSettings> CurrentView::named_clip_settings() const {
  if (clip_planes.empty() && clip_set.integers.empty() &&
      clip_set.numbers.empty() && clip_set.strings.empty() &&
      !clip_declared_plane_count)
    return std::nullopt;
  detail::require(wire_version >= 116
                      ? clip_planes.size() == 6
                      : !clip_planes.empty() && clip_planes.size() <= 6,
                  "inconsistent clip plane count");
  if (clip_declared_plane_count) {
    const auto count = *clip_declared_plane_count;
    detail::require(wire_version >= 116 ? count == 6
                                        : count <= 6 && std::max(count, 1u) ==
                                                            clip_planes.size(),
                    "inconsistent declared clip plane count");
  }
  return clip_settings_fields(clip_set, wire_version);
}
ViewerState ViewerState::scaled(double factor) const noexcept {
  auto result = *this;
  result.radius *= factor;
  result.height *= factor;
  result.actual_height *= factor;
  result.eye_height_offset *= factor;
  result.first_to_third_distance *= factor;
  if (result.gravity_value)
    *result.gravity_value *= factor;
  if (result.terminal_velocity)
    *result.terminal_velocity *= factor;
  return result;
}
std::optional<ViewerState> CurrentView::named_viewer() const {
  using detail::require;
  require(wire_version >= 46, "unsupported legacy viewpoint version");
  require((parts & ~0xfffu) == 0, "unknown viewpoint parts");
  if (!(parts & 2)) {
    require(viewer.numbers.empty() && viewer.integers.empty() &&
                viewer.strings.empty() && !viewer_avatar_is_null,
            "viewer fields without viewer part");
    return std::nullopt;
  }
  require(viewer.numbers.size() == (wire_version >= 50 ? 9u : 7u) &&
              viewer.integers.size() == (wire_version >= 55 ? 5u : 4u) &&
              viewer.strings.size() == 1,
          "inconsistent viewer field arrays");
  require(!viewer_avatar_is_null || viewer.strings[0].empty(),
          "nonempty NULL viewer avatar");
  auto boolean = [&](size_t i) {
    require(viewer.integers[i] <= 1, "viewer boolean");
    return viewer.integers[i] != 0;
  };
  ViewerState result;
  result.radius = viewer.numbers[0];
  result.height = viewer.numbers[1];
  result.actual_height = viewer.numbers[2];
  result.eye_height_offset = viewer.numbers[3];
  if (!viewer_avatar_is_null)
    result.avatar = viewer.strings[0];
  result.camera_mode = viewer.integers[0];
  result.first_to_third_angle = viewer.numbers[4];
  result.first_to_third_distance = viewer.numbers[5];
  result.first_to_third_param = viewer.numbers[6];
  result.first_to_third_correction = boolean(1);
  result.collision_detection = boolean(2);
  result.gravity = boolean(3);
  if (wire_version >= 50) {
    result.gravity_value = viewer.numbers[7];
    result.terminal_velocity = viewer.numbers[8];
  }
  if (wire_version >= 55)
    result.auto_crouch = boolean(4);
  return result;
}
ViewpointState CurrentView::named_state() const {
  using detail::require;
  require(wire_version >= 46, "unsupported legacy viewpoint version");
  require((parts & ~0xfffu) == 0, "unknown viewpoint parts");
  ViewpointState result;
  size_t ni = 0, ii = 0;
  auto number = [&] {
    require(ni < state.numbers.size(), "incomplete viewpoint numbers");
    return state.numbers[ni++];
  };
  auto integer = [&] {
    require(ii < state.integers.size(), "incomplete viewpoint integers");
    return state.integers[ii++];
  };
  if (parts & 4)
    result.world_up = std::array<double, 3>{number(), number(), number()};
  if (parts & 8)
    result.focal_distance = number();
  if (parts & 16)
    result.linear_speed = number();
  if (parts & 32)
    result.angular_speed = number();
  if (parts & 64) {
    result.serialized_tool = integer();
    result.tool = viewpoint_tool(*result.serialized_tool, wire_version);
  }
  if (parts & 128)
    result.tilt_limits = std::array<double, 2>{number(), number()};
  if (parts & 256) {
    result.lighting = integer();
    require(*result.lighting <= 3, "viewpoint lighting enum");
  }
  if (parts & 512) {
    result.render_style = integer();
    require(*result.render_style <= 4, "viewpoint render style enum");
  }
  if (parts & 1024)
    result.preferred_fov = number();
  if (parts & 2048)
    result.primitives = integer();
  if (wire_version >= 425) {
    ViewpointRenderSettings settings;
    settings.near_distance = number();
    settings.near_distance_type = integer();
    settings.far_distance = number();
    settings.far_distance_type = integer();
    settings.image_fit = integer();
    settings.horizontal_scale = number();
    settings.aperture_diameter = number();
    settings.shutter_speed = number();
    result.render_settings = settings;
  }
  require(ni == state.numbers.size() && ii == state.integers.size() &&
              state.strings.empty(),
          "inconsistent viewpoint state arrays");
  return result;
}
Camera detail::read_camera(Cursor &r, uint32_t version) {
  Camera c;
  c.projection = r.u32();
  require(c.projection <= 1, "camera projection enum");
  for (auto &x : c.position)
    x = r.f64();
  for (auto &x : c.orientation)
    x = r.f64();
  c.offset_factors_present = version >= 425;
  for (unsigned i = 0; i < (c.offset_factors_present ? 6u : 4u); ++i)
    c.parameters[i] = r.read<double>();
  return c;
}
CurrentView detail::read_viewpoint(Cursor &r, uint32_t version) {
  using namespace detail;
  require(version >= 46, "unsupported legacy viewpoint version");
  CurrentView v;
  v.wire_version = version;
  auto doubles = [&](ViewFields &f, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
      f.numbers.push_back(r.read<double>());
  };
  auto integer = [&](ViewFields &f) {
    auto value = r.u32();
    f.integers.push_back(value);
    return value;
  };
  v.parts = r.u32();
  require((v.parts & ~0xfffu) == 0, "unknown viewpoint parts");
  if (v.parts & 1) {
    v.camera = read_camera(r, version);
  }
  if (v.parts & 2) {
    auto &f = v.viewer;
    doubles(f, 4);
    f.strings.push_back(r.string(&v.viewer_avatar_is_null));
    integer(f);
    doubles(f, 3);
    for (unsigned i = 0; i < 3; ++i)
      require(integer(f) <= 1, "viewer boolean");
    if (version >= 50)
      doubles(f, 2);
    if (version >= 55)
      require(integer(f) <= 1, "viewer boolean");
  }
  auto &f = v.state;
  if (v.parts & 4)
    doubles(f, 3);
  if (v.parts & 8)
    doubles(f, 1);
  if (v.parts & 16)
    doubles(f, 1);
  if (v.parts & 32)
    doubles(f, 1);
  if (v.parts & 64)
    viewpoint_tool(integer(f), version);
  if (v.parts & 128)
    doubles(f, 2);
  if (v.parts & 256)
    require(integer(f) <= 3, "viewpoint lighting enum");
  if (v.parts & 512)
    require(integer(f) <= 4, "viewpoint render style enum");
  if (v.parts & 1024)
    doubles(f, 1);
  if (v.parts & 2048)
    integer(f);
  if (version >= 425) {
    doubles(f, 1);
    integer(f);
    doubles(f, 1);
    integer(f);
    integer(f);
    doubles(f, 3);
  }
  return v;
}
CurrentView detail::read_current_view(Cursor &r, uint32_t version) {
  auto v = read_viewpoint(r, version);
  v.clip_declared_plane_count =
      read_clip_planes(r, v.clip_planes, v.clip_set, version);
  return v;
}
uint32_t detail::read_clip_planes(Cursor &r, std::vector<ViewFields> &planes,
                                  ViewFields &clips, uint32_t version) {
  if (version < 18)
    throw UnsupportedLayout("legacy clip range layout before version18");
  auto doubles = [&](ViewFields &f, unsigned n) {
    while (n--)
      f.numbers.push_back(r.read<double>());
  };
  auto integer = [&](ViewFields &f) {
    auto v = r.u32();
    f.integers.push_back(v);
    return v;
  };
  auto plane = [&] {
    ViewFields p;
    const auto state = integer(p);
    require(version >= 116 || state <= 1, "legacy clip plane boolean");
    integer(p);
    doubles(p, version >= 116 ? 9 : 5);
    planes.push_back(std::move(p));
  };
  uint32_t declared_count;
  if (version >= 116) {
    declared_count = r.u32();
    require(declared_count == 6, "clip plane count");
    for (unsigned i = 0; i < 6; ++i)
      plane();
  } else {
    plane();
    declared_count = r.u32();
    require(declared_count <= 6, "legacy clip plane count");
    for (unsigned i = 1; i < declared_count; ++i)
      plane();
  }
  require(integer(clips) <= 1, "clip set boolean");
  integer(clips);
  doubles(clips, 6);
  if (version >= 107) {
    integer(clips);
    require(integer(clips) <= 1, "clip box boolean");
    doubles(clips, 6);
  }
  if (version >= 118)
    doubles(clips, 4);
  return declared_count;
}
CurrentView decode_current_view(std::span<const uint8_t> data,
                                uint32_t version) {
  detail::Cursor r(data, "current viewpoint");
  auto v = detail::read_current_view(r, version);
  r.exact();
  return v;
}
} // namespace nwd
