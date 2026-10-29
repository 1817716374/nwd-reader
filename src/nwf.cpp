#include "internal.hpp"
#include "json.hpp"
namespace nwd {
using namespace detail;
ExternalReferenceTable detail::read_xref_table(Cursor &r,
                                               const Options &options) {
  ExternalReferenceTable out;
  out.json = r.string();
  try {
    auto j = nlohmann::json::parse(out.json);
    require(j.at("Type") == "XRefTable" && j.at("Version") == 1,
            "unsupported XRef table version");
    out.saved_filename = j.value("SavedFilename", std::string{});
    const auto &table = j.at("Table");
    require(table.is_array() && table.size() <= options.max_objects,
            "XRef table array/resource limit");
    for (const auto &x : table)
      out.path_remaps.emplace_back(x.at("OriginalPath").get<std::string>(),
                                   x.at("RemappedPath").get<std::string>());
  } catch (const nlohmann::json::exception &e) {
    throw Error(std::string("invalid XRef table: ") + e.what());
  }
  return out;
}
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
void detail::read_cache_data(Cursor &r, std::vector<CachePlugin> &cache_plugins,
                             std::vector<CachedReference> &cached_files,
                             std::vector<CacheOption> &cache_options,
                             ObjectReader &object_reader,
                             const Options &options, uint64_t &budget) {
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
  cache_plugins.push_back(plugin());
  for (auto c = count(); c; --c)
    cache_plugins.push_back(plugin());
  for (auto c = count(); c; --c) {
    CachedReference f;
    f.name = r.string(&f.name_is_null);
    r.align(4);
    f.path = r.string(&f.path_is_null);
    if (!f.path_is_null) {
      f.timestamp = r.u64();
      f.size = r.u64();
    }
    cached_files.push_back(std::move(f));
  }
  for (auto c = count(); c; --c) {
    CacheOption o;
    o.name = r.string();
    o.value = data_value();
    cache_options.push_back(std::move(o));
  }
}
static void read_legacy_xrefs(Cursor &r,
                              std::vector<CachedReference> &cached_files,
                              uint32_t version, const Options &options,
                              uint64_t &budget, bool skip_null_names) {
  const auto xref_count = r.u32();
  require(xref_count <= std::min<uint64_t>(1000000, options.max_objects),
          "legacy NWF cache count limit");
  require(xref_count <= budget, "legacy NWF cache resource limit");
  budget -= xref_count;
  cached_files.reserve(cached_files.size() + xref_count);
  for (uint32_t i = 0; i < xref_count; ++i) {
    CachedReference file;
    file.name = r.string(&file.name_is_null);
    // NW5 validation counts null names but does not construct an xref. The
    // preceding xref-only encoding still reads the full xref object.
    if (skip_null_names && file.name_is_null)
      continue;
    file.path = r.string(&file.path_is_null);
    if (version >= 62 && !file.path_is_null) {
      file.timestamp = r.u64();
      file.size = r.u64();
    }
    cached_files.push_back(std::move(file));
  }
}

static void read_legacy_cache_data(Cursor &r, NwfReference &reference,
                                   ObjectReader &object_reader,
                                   uint32_t version, const Options &options,
                                   uint64_t &budget) {
  reference.legacy_cache_encoding = true;
  reference.cache_format = NwfCacheFormat::nw5_validation;
  reference.cache_validation_plugin = r.string();
  reference.cache_validation_version = r.read<int32_t>();
  if (version >= 72)
    reference.collapse_type = r.read<int32_t>();

  // SceneSet calls NW5 only from version72. That branch is count-bounded and
  // null names consume a slot without constructing an xref.
  read_legacy_xrefs(r, reference.cached_files, version, options, budget, true);

  reference.legacy_option_value_count = r.read<int32_t>();
  if (reference.legacy_option_value_count <= 0)
    return;
  const auto value_count =
      static_cast<uint32_t>(reference.legacy_option_value_count);
  require(value_count <= std::min<uint64_t>(1000000, options.max_objects),
          "legacy NWF cache option count limit");
  require(value_count <= budget,
          "legacy NWF cache option resource limit");
  budget -= value_count;
  reference.legacy_option_values.reserve(value_count);
  for (uint32_t i = 0; i < value_count; ++i)
    reference.legacy_option_values.push_back(read_data_value(
        r, [&] { return object_reader.string(r); },
        [&] { return Reference{0, object_reader.object(r)}; }));
}
void detail::read_source_references(Cursor &r,
                                    std::vector<NwfReference> &references,
                                    ObjectReader &object_reader,
                                    uint32_t version, const Options &options) {
  require(version <= 450,
          "source-reference version not yet validated");
  auto count = [&]() {
    auto n = r.u32();
    require(n <= std::min<uint64_t>(1000000, options.max_objects),
            "NWF count limit");
    return n;
  };
  uint64_t budget = std::min<uint64_t>(1000000, options.max_objects);
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
    if (version < 64) {
      x.legacy_override_patch = NwfLegacyOverridePatch{r.u32(), r.u32()};
    }
    x.flags = r.u32();
    x.load_flags = version >= 244 ? r.u32() : 7;
    for (auto &v : x.affine)
      v = r.f64();
    if (version >= 5) {
      x.linear_units = r.u32();
      x.angular_units = r.u32();
      x.orientation_flag = r.u32();
    } else {
      // Native LcOpSceneRef constructor/read defaults for old streams.
      x.angular_units = 1;
    }
    if (version >= 53) {
      x.up = vector3(r);
      x.north = vector3(r);
    }
    if (version >= 80) {
      x.extra_enum = r.u32();
      x.extra = r.string();
    }
    if (version >= 63 && version < 72) {
      x.legacy_cache_encoding = true;
      x.cache_format = NwfCacheFormat::legacy_xrefs;
      read_legacy_xrefs(r, x.cached_files, version, options, budget, false);
    } else if (version >= 72 && version < 92)
      read_legacy_cache_data(r, x, object_reader, version, options, budget);
    else if (version >= 92) {
      x.cache_format = NwfCacheFormat::plugin_options;
      read_cache_data(r, x.cache_plugins, x.cached_files, x.cache_options,
                      object_reader, options, budget);
    }
    if (version >= 109)
      x.front = vector3(r);
    if (version >= 246)
      x.reference_guid = guid(r);
    references.push_back(std::move(x));
  }
}
NwfData decode_nwf_scene_set(std::span<const uint8_t> bytes, uint32_t version,
                             Options options) {
  require(version <= 448,
          "NWF scene-set version not yet validated");
  Cursor r(bytes, "NWF scene set");
  NwfData out;
  // Native SceneSet reader uses these unit defaults before version5.
  out.angular_units = 1;
  ObjectReader objects(bytes, out.option_values, version, options);
  read_source_references(r, out.references, objects, version, options);
  if (version >= 5) {
    out.linear_units = r.u32();
    out.angular_units = r.u32();
  }
  if (version >= 53) {
    out.up = vector3(r);
    out.north = vector3(r);
  }
  if (version >= 109)
    out.front = vector3(r);
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
void detail::read_texture_spaces(Cursor &r,
                                 std::vector<NwfTextureSpace> &records,
                                 uint32_t version, bool implicit_node_map,
                                 const Options &options) {
  uint64_t budget = std::min<uint64_t>(1000000, options.max_objects);
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
      t.paths = read_path_selector(r, implicit_node_map, scope != 0, budget);
      t.value = space();
      records.push_back(std::move(t));
    }
  }
}
void decode_nwf_texture_spaces(NwfData &out, std::span<const uint8_t> bytes,
                               uint32_t version) {
  Cursor r(bytes, "NWF texture spaces");
  std::vector<NwfTextureSpace> records;
  read_texture_spaces(r, records, version, true, Options{});
  r.exact();
  out.texture_spaces = std::move(records);
}
NwfData Document::read_nwf() const {
  NwfData out;
  bool found = false;
  for (size_t i = 0; i < chunks().size(); ++i)
    if (chunks()[i].name.ends_with("LcOpNwfSceneSet")) {
      require(!found, "multiple NWF scene sets unsupported");
      out = decode_nwf_scene_set(read_chunk(i), version(), options());
      found = true;
    }
  require(found, "not an NWF scene set");
  out.parsed_chunks.resize(chunks().size());
  for (size_t i = 0; i < chunks().size(); ++i)
    if (chunks()[i].name.ends_with("LcOpNwfSceneSet"))
      out.parsed_chunks[i] = true;
  // Resolve selectors independently of directory order.
  bool have_path_map = false;
  for (size_t i = 0; i < chunks().size(); ++i)
    if (chunks()[i].name.ends_with("LcOpNwfPathMap")) {
      require(!have_path_map, "multiple NWF path maps need separate contexts");
      have_path_map = true;
      const auto &name = chunks()[i].name;
      auto split = name.rfind('\\');
      out.path_map_namespace = split == name.npos ? "" : name.substr(0, split);
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
      auto table = read_xref_table(r, options());
      r.exact();
      out.xref_json = std::move(table.json);
      out.saved_filename = std::move(table.saved_filename);
      out.path_remaps = std::move(table.path_remaps);
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
