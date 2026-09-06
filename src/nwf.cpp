#include "internal.hpp"
#include "json.hpp"
namespace nwd {
using namespace detail;
static std::array<uint8_t, 16> guid(Cursor &r) {
  r.align(4);
  std::array<uint8_t, 16> a;
  auto b = r.raw(16);
  std::copy(b.begin(), b.end(), a.begin());
  return a;
}
static std::array<double, 3> vector3(Cursor &r) {
  return {r.f64(), r.f64(), r.f64()};
}
NwfData decode_nwf_scene_set(std::span<const uint8_t> bytes, uint32_t version) {
  require(version >= 251 && version <= 448,
          "NWF scene-set version not yet validated");
  Cursor r(bytes, "NWF scene set");
  NwfData out;
  auto count = [&]() {
    auto n = r.u32();
    require(n < 1000000, "NWF count limit");
    return n;
  };
  ObjectReader object_reader(bytes, out.option_values, version);
  auto data_value = [&]() {
    return read_data_value(
        r, [&] { return object_reader.string(r); },
        [&] { return Reference{0, object_reader.object(r)}; });
  };
  size_t budget = 1000000;
  std::function<void(CacheOption &, unsigned)> option_set =
      [&](CacheOption &set, unsigned depth) {
        require(depth < 128, "cache option nesting limit");
        set.flags = r.u32();
        if (set.flags & 8)
          return;
        auto n = count();
        require(n <= budget, "cache option count limit");
        budget -= n;
        set.children.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
          CacheOption o;
          o.name = r.string();
          o.value = data_value();
          if (o.value.tag == 0)
            option_set(o, depth + 1);
          set.children.push_back(std::move(o));
        }
      };
  auto plugin = [&]() {
    CachePlugin p;
    p.name = r.string();
    p.version = r.read<int32_t>();
    auto has = r.u32();
    require(has <= 1, "invalid cache option boolean");
    p.has_options = has != 0;
    if (p.has_options)
      option_set(p.options, 0);
    return p;
  };
  auto n = count();
  out.references.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    NwfReference x;
    x.name = r.string();
    x.original_path = r.string();
    x.partition = r.string();
    x.source_guid = guid(r);
    x.display_name = r.string();
    x.plugin = r.string();
    x.flags = r.u32();
    x.load_flags = r.u32();
    for (auto &v : x.affine)
      v = r.f64();
    x.linear_units = r.u32();
    x.angular_units = r.u32();
    x.orientation_flag = r.u32();
    x.up = vector3(r);
    x.front = vector3(r);
    x.extra_enum = r.u32();
    x.extra = r.string();
    x.cache_plugins.push_back(plugin());
    for (auto c = count(); c; --c)
      x.cache_plugins.push_back(plugin());
    for (auto c = count(); c; --c) {
      CachedReference f;
      f.name = r.string();
      r.align(4);
      uint32_t len;
      require(r.pos + 4 <= bytes.size(), "truncated NWF cache path");
      std::memcpy(&len, bytes.data() + r.pos, 4);
      f.path = r.string();
      if (len != UINT32_MAX) {
        f.timestamp = r.u64();
        f.size = r.u64();
      }
      x.cached_files.push_back(std::move(f));
    }
    for (auto c = count(); c; --c) {
      CacheOption o;
      o.name = r.string();
      o.value = data_value();
      x.cache_options.push_back(std::move(o));
    }
    x.north = vector3(r);
    x.reference_guid = guid(r);
    out.references.push_back(std::move(x));
  }
  out.linear_units = r.u32();
  out.angular_units = r.u32();
  out.up = vector3(r);
  out.front = vector3(r);
  out.north = vector3(r);
  r.exact();
  return out;
}
void decode_nwf_path_map(NwfData &out, std::span<const uint8_t> bytes) {
  Cursor r(bytes, "NWF path map");
  out.path_map_kind = r.u32();
  require(out.path_map_kind <= 2, "unknown NWF path map kind");
  out.paths.resize(out.path_map_kind ? 2 : 1);
  if (out.path_map_kind < 2) {
    r.exact();
    return;
  }
  out.path_map_flags = r.u32();
  require((out.path_map_flags & ~0x3ffu) == 0, "unknown path map fields");
  std::vector<Id> stack{1};
  for (;;) {
    auto signed_depth = r.read<int32_t>();
    if (!signed_depth)
      break;
    require(signed_depth != INT32_MIN, "invalid path depth");
    auto depth = uint32_t(std::abs(signed_depth));
    require(depth <= stack.size() && depth < 512, "invalid NWF path depth");
    stack.resize(depth);
    NwfPath p;
    p.parent = stack.back();
    p.flags = r.u32();
    require((p.flags & ~out.path_map_flags) == 0,
            "path fields outside map mask");
    if (p.flags & 64)
      p.sibling = r.read<int32_t>();
    if (p.flags & 1)
      p.type = r.u32();
    if (p.flags & 2)
      p.class_name = r.string();
    if (p.flags & 4)
      p.name = r.string();
    if (p.flags & 512)
      p.guid = guid(r);
    if (p.flags & 8)
      p.entity = r.u64();
    if (p.flags & 128)
      p.source = r.string();
    if (p.flags & 256) {
      auto n = r.u32();
      require(n <= 1000000, "path fragment checksum count");
      for (uint32_t j = 0; j < n; ++j)
        p.fragment_checksums.push_back(r.u32());
    }
    if (signed_depth < 0) {
      for (unsigned bit : {16u, 32u})
        if (out.path_map_flags & bit)
          for (unsigned j = 0; j < 6; ++j)
            p.bounds.push_back(r.read<double>());
    }
    require(out.paths.size() < 1000000, "NWF path map resource limit");
    auto id = static_cast<Id>(out.paths.size());
    out.paths.push_back(std::move(p));
    if (signed_depth > 0)
      stack.push_back(id);
  }
  r.exact();
}
void decode_nwf_transforms(NwfData &out, std::span<const uint8_t> bytes) {
  Cursor r(bytes, "NWF fragment transform overrides");
  auto count = r.u32();
  require(count <= 1000000, "NWF transform override count limit");
  std::vector<NwfTransformOverride> records;
  records.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    NwfTransformOverride t;
    t.path = r.u32();
    t.flags = r.u32();
    unsigned size = !t.flags           ? 0
                    : (t.flags & 32)   ? 16
                    : !(t.flags & ~7u) ? 9
                    : t.flags == 16    ? 3
                                       : 12;
    t.values.reserve(size);
    for (unsigned j = 0; j < size; ++j)
      t.values.push_back(r.f64());
    records.push_back(std::move(t));
  }
  r.exact();
  out.transform_overrides = std::move(records);
}
NwfData Document::read_nwf() const {
  NwfData out;
  bool found = false;
  for (size_t i = 0; i < chunks().size(); ++i)
    if (chunks()[i].name.ends_with("LcOpNwfSceneSet")) {
      require(!found, "multiple NWF scene sets unsupported");
      out = decode_nwf_scene_set(read_chunk(i), version());
      found = true;
    }
  require(found, "not an NWF scene set");
  for (size_t i = 0; i < chunks().size(); ++i) {
    auto name = chunks()[i].name;
    if (name.ends_with("LcOpNwfPathMap"))
      decode_nwf_path_map(out, read_chunk(i));
    else if (name.ends_with("LcOpShadFragMaterialElement"))
      read_nwf_appearances(out, read_chunk(i), version(), options());
    else if (name.ends_with("LcOpShadFragTransformElement2"))
      decode_nwf_transforms(out, read_chunk(i));
    else if (name.ends_with("LcOpTextureSpaceElement")) {
      auto b = read_chunk(i);
      Cursor r(b, "NWF transform overrides");
      if (r.u32() != 0)
        out.unparsed_core.push_back(name);
      else
        r.exact();
    } else if (name == "LcOpXRefTable") {
      auto b = read_chunk(i);
      Cursor r(b);
      out.xref_json = r.string();
      r.exact();
      try {
        auto j = nlohmann::json::parse(out.xref_json);
        require(j.at("Type") == "XRefTable" && j.at("Version") == 1,
                "unsupported XRef table version");
        out.saved_filename = j.value("SavedFilename", std::string{});
        for (const auto &x : j.at("Table"))
          out.path_remaps.emplace_back(x.at("OriginalPath").get<std::string>(),
                                       x.at("RemappedPath").get<std::string>());
      } catch (const nlohmann::json::exception &e) {
        throw Error(std::string("invalid XRef table: ") + e.what());
      }
    }
  }
  for (const auto &a : out.appearance_overrides)
    for (auto p : a.paths)
      require(p == none || p < out.paths.size(),
              "appearance override outside NWF path map");
  for (const auto &t : out.transform_overrides)
    require(t.path == 0 || t.path < out.paths.size(),
            "transform override outside NWF path map");
  return out;
}
} // namespace nwd
