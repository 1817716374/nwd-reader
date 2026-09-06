#include "internal.hpp"
namespace nwd {
Camera detail::read_camera(Cursor &r, uint32_t version) {
  Camera c;
  c.projection = r.u32();
  require(c.projection <= 1, "camera projection enum");
  for (auto &x : c.position)
    x = r.f64();
  for (auto &x : c.orientation)
    x = r.f64();
  for (unsigned i = 0; i < (version >= 425 ? 6u : 4u); ++i)
    c.parameters[i] = r.read<double>();
  return c;
}
CurrentView detail::read_current_view(Cursor &r, uint32_t version) {
  using namespace detail;
  require(version >= 46, "unsupported legacy viewpoint version");
  CurrentView v;
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
    f.strings.push_back(r.string());
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
    integer(f);
  if (v.parts & 128)
    doubles(f, 2);
  if (v.parts & 256)
    integer(f);
  if (v.parts & 512)
    integer(f);
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
  read_clip_planes(r, v.clip_planes, v.clip_set, version);
  return v;
}
void detail::read_clip_planes(Cursor &r, std::vector<ViewFields> &planes,
                              ViewFields &clips, uint32_t version) {
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
    integer(p);
    integer(p);
    doubles(p, version >= 116 ? 9 : 5);
    planes.push_back(std::move(p));
  };
  if (version >= 116) {
    require(r.u32() == 6, "clip plane count");
    for (unsigned i = 0; i < 6; ++i)
      plane();
  } else {
    plane();
    auto count = r.u32();
    require(count >= 1 && count <= 6, "legacy clip plane count");
    for (unsigned i = 1; i < count; ++i)
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
}
CurrentView decode_current_view(std::span<const uint8_t> data,
                                uint32_t version) {
  detail::Cursor r(data, "current viewpoint");
  auto v = detail::read_current_view(r, version);
  r.exact();
  return v;
}
} // namespace nwd
