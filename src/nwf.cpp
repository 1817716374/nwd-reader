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
void detail::read_source_references(Cursor &r,
                                    std::vector<NwfReference> &references,
                                    ObjectReader &object_reader,
                                    uint32_t version, const Options &options) {
  require(version >= 92 && version <= 450,
          "source-reference version not yet validated");
  auto bytes = r.data;
  auto count = [&]() {
    auto n = r.u32();
    require(n <= std::min<uint64_t>(1000000, options.max_objects),
            "NWF count limit");
    return n;
  };
  auto data_value = [&]() {
    return read_data_value(
        r, [&] { return object_reader.string(r); },
        [&] { return Reference{0, object_reader.object(r)}; });
  };
  size_t budget = std::min<uint64_t>(1000000, options.max_objects);
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
  references.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    NwfReference x;
    x.name = r.string();
    x.original_path = r.string();
    if (version >= 217)
      x.partition = r.string();
    if (version >= 251) {
      x.source_guid = guid(r);
      x.display_name = r.string();
    }
    x.plugin = r.string();
    x.flags = r.u32();
    if (version >= 244)
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
    if (version >= 109)
      x.north = vector3(r);
    if (version >= 246)
      x.reference_guid = guid(r);
    references.push_back(std::move(x));
  }
}
NwfData decode_nwf_scene_set(std::span<const uint8_t> bytes, uint32_t version) {
  require(version >= 251 && version <= 448,
          "NWF scene-set version not yet validated");
  Cursor r(bytes, "NWF scene set");
  NwfData out;
  ObjectReader objects(bytes, out.option_values, version);
  read_source_references(r, out.references, objects, version);
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
void decode_nwf_texture_spaces(NwfData &out, std::span<const uint8_t> bytes,
                               uint32_t version) {
  Cursor r(bytes, "NWF texture spaces");
  std::vector<NwfTextureSpace> records;
  size_t budget = 1000000;
  auto count = [&]() {
    auto n = r.u32();
    require(n <= budget, "NWF texture space resource limit");
    budget -= n;
    return n;
  };
  auto space = [&]() {
    TextureSpace s;
    auto type = r.u32();
    require(type <= 4, "unknown NWF texture mapping");
    s.mapping = static_cast<TextureMapping>(type);
    if (version < 428 || type == 4)
      return s;
    s.parameters_present = true;
    s.vectors.push_back(vector3(r));
    s.vectors.push_back(vector3(r));
    for (auto &v : s.rotation)
      v = r.f64();
    if (type == 2) {
      auto flag = r.u32();
      require(flag <= 1, "invalid texture cylinder boolean");
      s.cylinder_flag = flag != 0;
      s.cylinder_cap_threshold = r.f64();
    }
    if (type <= 2) {
      s.vectors.push_back(vector3(r));
      s.vectors.push_back(vector3(r));
    }
    const unsigned enums = type == 0 ? 12 : type == 2 ? 4 : 0;
    for (unsigned i = 0; i < enums; ++i)
      s.directions.push_back(r.u32());
    return s;
  };
  for (unsigned scope = 0; scope < 2; ++scope) {
    auto n = count();
    for (uint32_t i = 0; i < n; ++i) {
      NwfTextureSpace t;
      t.node_scope = scope != 0;
      // NWF ReadContentsImplicit leaves the native map's explicit-mode flag
      // clear for all three map kinds. ReadNode therefore reads a sentinel-
      // terminated candidate list, even when the path map kind is 2.
      if (scope) {
        for (;;) {
          auto id = r.u32();
          if (id == none)
            break;
          require(budget > 0, "NWF texture node lookup resource limit");
          --budget;
          t.paths.push_back(id);
        }
      } else
        t.paths.push_back(r.u32());
      t.value = space();
      records.push_back(std::move(t));
    }
  }
  r.exact();
  out.texture_spaces = std::move(records);
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
  out.parsed_chunks.resize(chunks().size());
  for (size_t i = 0; i < chunks().size(); ++i)
    if (chunks()[i].name.ends_with("LcOpNwfSceneSet"))
      out.parsed_chunks[i] = true;
  // Resolve selectors independently of directory order.
  for (size_t i = 0; i < chunks().size(); ++i)
    if (chunks()[i].name.ends_with("LcOpNwfPathMap")) {
      decode_nwf_path_map(out, read_chunk(i));
      out.parsed_chunks[i] = true;
    }
  for (size_t i = 0; i < chunks().size(); ++i) {
    auto name = chunks()[i].name;
    if (name.ends_with("LcOpShadFragMaterialElement"))
      read_nwf_appearances(out, read_chunk(i), version(), options());
    else if (name.ends_with("LcOpShadFragTransformElement2"))
      decode_nwf_transforms(out, read_chunk(i));
    else if (name.ends_with("LcOpTextureSpaceElement")) {
      decode_nwf_texture_spaces(out, read_chunk(i), version());
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
    } else
      continue;
    out.parsed_chunks[i] = true;
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
