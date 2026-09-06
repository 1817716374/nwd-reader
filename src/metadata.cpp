#include "internal.hpp"
namespace nwd::detail {
class GraphReader {
  Cursor r;
  ObjectGraph &graph;
  Id graph_id;
  uint32_t version;
  const Options &options;
  bool root_partition;
  std::vector<Id> ids;
  std::unordered_map<std::string, Id> strings;
  unsigned depth = 0;
  uint32_t count() {
    auto n = r.u32();
    require(n <= options.max_objects, "metadata count resource limit");
    return n;
  }
  Id str() {
    r.align(4);
    require(r.pos + 4 <= r.data.size(), "truncated string length");
    uint32_t n;
    std::memcpy(&n, r.data.data() + r.pos, 4);
    if (n == UINT32_MAX) {
      r.pos += 4;
      return none;
    }
    auto s = r.string();
    auto it = strings.find(s);
    if (it != strings.end())
      return it->second;
    Id i = static_cast<Id>(graph.strings.size());
    graph.strings.push_back(s);
    strings.emplace(std::move(s), i);
    return i;
  }
  Reference ref() { return {graph_id, object()}; }
  void common(Object &o) {
    o.name = str();
    o.class_name = object();
    o.flags = r.u32();
  }
  void doubles(Object &o, unsigned n) {
    for (unsigned j = 0; j < n; ++j)
      o.numbers.push_back(r.f64());
  }
  Shader shader() {
    Shader s;
    s.name = str();
    auto n = count();
    for (uint32_t j = 0; j < n; ++j) {
      ShaderArgument a;
      a.name = str();
      auto &v = a.value;
      v.tag = r.u32();
      switch (v.tag) {
      case 1:
        v.integer = r.read<int32_t>();
        break;
      case 2:
      case 5:
      case 6:
        v.integer = r.u32();
        break;
      case 3:
        v.number[0] = r.read<double>();
        break;
      case 7:
      case 8:
      case 9:
        for (auto &x : v.number)
          x = r.read<double>();
        break;
      case 10:
        v.string = str();
        break;
      default:
        r.fail("unsupported shader argument " + std::to_string(v.tag));
      }
      s.arguments.push_back(std::move(a));
    }
    return s;
  }
  ProteinProperty protein(Id owner) {
    ProteinProperty p;
    p.type = r.u32();
    p.flags = r.u32();
    p.name = str();
    p.count = count();
    p.enabled = r.u32() != 0;
    auto n = version >= 242 ? count() : 1;
    for (uint32_t j = 0; j < n; ++j)
      p.connections.push_back(ref());
    switch (p.type) {
    case 0:
    case 11:
      break;
    case 1: {
      auto b = r.raw(p.count);
      p.bytes.assign(b.begin(), b.end());
      break;
    }
    case 2:
    case 6:
      p.integers.push_back(r.read<int32_t>());
      break;
    case 7:
      for (uint32_t j = 0; j < p.count; ++j)
        p.integers.push_back(r.read<int32_t>());
      break;
    case 3:
    case 4:
    case 8:
    case 9: {
      uint64_t n = p.type == 8 ? 1
                               : uint64_t(p.count) * (p.type == 3   ? 3
                                                      : p.type == 4 ? 4
                                                                    : 1);
      require(n <= options.max_objects, "protein array limit");
      for (uint64_t j = 0; j < n; ++j)
        p.numbers.push_back(r.f64());
      break;
    }
    case 5:
    case 10:
    case 12:
      p.strings.push_back(str());
      break;
    case 16:
      if (version >= 242)
        p.strings.push_back(str());
      break;
    case 13:
      for (uint32_t j = 0; j < p.count; ++j) {
        auto name = str();
        p.strings.push_back(name);
        auto embedded = r.u32();
        require(embedded <= 1, "invalid protein URI embedded flag");
        if (embedded) {
          auto file = read_embedded_asset(
              r, name == none ? std::string{} : graph.strings[name], owner, j);
          p.embedded_files.push_back(
              static_cast<Id>(graph.embedded_files.size()));
          graph.embedded_files.push_back(std::move(file));
        }
      }
      break;
    case 14: {
      auto file = read_embedded_asset(r, {}, owner, 0);
      p.embedded_files.push_back(static_cast<Id>(graph.embedded_files.size()));
      graph.embedded_files.push_back(std::move(file));
      break;
    }
    case 15:
      if (version < 242)
        break;
      p.integers.push_back(r.read<int32_t>());
      for (uint32_t j = 0; j < p.count; ++j) {
        p.integers.push_back(r.read<int32_t>());
        p.strings.push_back(str());
      }
      break;
    default:
      r.fail("unsupported protein property type " + std::to_string(p.type));
    }
    return p;
  }
  Value value() {
    return read_data_value(r, [&] { return str(); }, [&] { return ref(); });
  }
  void animation(uint32_t type, Id owner) {
    const auto slot = graph.animation_objects.size();
    graph.animation_objects.emplace_back();
    graph.animation_objects[slot].owner = owner;
    graph.animation_objects[slot].kind = static_cast<AnimationObjectKind>(type);
    AnimationObject out;
    out.owner = owner;
    out.kind = static_cast<AnimationObjectKind>(type);
    auto path = [&] {
      auto first = r.string();
      auto second = r.string();
      return std::pair(std::move(first), std::move(second));
    };
    auto boolean = [&] {
      auto raw = r.u32();
      require(raw <= 1, "animation object boolean");
      return raw != 0;
    };
    auto selection = [&](SavedSelection &v) {
      read_find_selection(r, v, options.max_objects);
    };
    switch (type) {
    case 150:
    case 154:
    case 163:
    case 165: {
      auto &v = out.value.emplace<AnimationPathRecord>();
      v.item_path = path();
      if (type != 150)
        v.mode = r.read<int32_t>();
      break;
    }
    case 151:
    case 162: {
      auto &v = out.value.emplace<AnimationVariableRecord>();
      v.name = r.string();
      if (type == 162)
        v.operation = r.read<int32_t>();
      v.value = value();
      if (type == 151)
        v.operation = r.read<int32_t>();
      break;
    }
    case 152:
    case 164: {
      auto &v = out.value.emplace<AnimationTimeRecord>();
      v.delay = r.f64();
      if (type == 164)
        v.mode = r.read<int32_t>();
      break;
    }
    case 153: {
      auto &v = out.value.emplace<AnimationPlayRecord>();
      v.animation_path = path();
      v.finish = boolean();
      v.start_type = r.read<int32_t>();
      v.end_type = r.read<int32_t>();
      v.start_time = r.f64();
      v.end_time = r.f64();
      break;
    }
    case 155:
    case 156:
      out.value.emplace<AnimationTextRecord>().text = r.string();
      break;
    case 157: {
      auto &v = out.value.emplace<AnimationPropertyRecord>();
      selection(v.selection);
      v.variable = r.string();
      v.category = r.string();
      v.property = r.string();
      break;
    }
    case 158: {
      auto &v = out.value.emplace<AnimationKeyRecord>();
      v.key = r.read<uint16_t>();
      v.mode = r.read<int32_t>();
      break;
    }
    case 159: {
      auto &v = out.value.emplace<AnimationSphereRecord>();
      for (auto &x : v.center)
        x = r.f64();
      v.radius = r.f64();
      break;
    }
    case 160: {
      auto &v = out.value.emplace<AnimationSelectionSphereRecord>();
      selection(v.selection);
      v.radius = r.f64();
      break;
    }
    case 161: {
      auto &v = out.value.emplace<AnimationHotspotRecord>();
      v.hotspot = ref();
      if (v.hotspot.object != none) {
        auto target = graph.objects.at(v.hotspot.object).type;
        require(target == 159 || target == 160,
                "animation hotspot reference type");
      }
      v.mode = r.read<int32_t>();
      break;
    }
    case 166: {
      auto &v = out.value.emplace<AnimationCollisionRecord>();
      selection(v.selection);
      v.gravity = boolean();
      break;
    }
    case 167:
      break;
    default:
      r.fail("unsupported animation object type");
    }
    // Reserving the slot before following references keeps owners sorted even
    // when an event contains another animation object (its hotspot).
    graph.animation_objects[slot] = std::move(out);
  }

public:
  GraphReader(std::span<const uint8_t> b, ObjectGraph &g, Id gid, uint32_t ver,
              const Options &op, bool root = false)
      : r(b, "metadata graph " + std::to_string(gid)), graph(g), graph_id(gid),
        version(ver), options(op), root_partition(root) {}
  Id object() {
    require(++depth < 512, "metadata recursion limit");
    struct Guard {
      unsigned &d;
      ~Guard() { --d; }
    } guard{depth};
    size_t start = r.pos;
    uint32_t id = r.u32();
    if (id == 0)
      return none;
    if (id == 1) {
      id = r.u32();
      require(id < ids.size() && ids[id] != none,
              "missing metadata object reference");
      return ids[id];
    }
    if (id < 100 || id >= options.max_objects)
      r.fail("metadata ID limit: " + std::to_string(id));
    if (id >= ids.size())
      ids.resize(id + 1, none);
    if (ids[id] != none)
      r.fail("duplicate metadata ID " + std::to_string(id));
    Id index = static_cast<Id>(graph.objects.size());
    ids[id] = index;
    graph.objects.emplace_back();
    Object o;
    o.type = r.u32();
    o.stream_offset = start;
    switch (o.type) {
    case 150:
    case 151:
    case 152:
    case 153:
    case 154:
    case 155:
    case 156:
    case 157:
    case 158:
    case 159:
    case 160:
    case 161:
    case 162:
    case 163:
    case 164:
    case 165:
    case 166:
    case 167:
      animation(o.type, index);
      break;
    case 89: {
      // LcOpElementRecord shares the stream object arena with name objects.
      // The node override copy is a zero-terminated (flags, path-link) list.
      require(version >= 82, "legacy element record layout");
      auto name = str();
      o.strings.push_back(name);
      if (name == none)
        break;
      const auto element = graph.strings[name];
      if (element == "LcOpShadOverridesElement") {
        for (;;) {
          auto flags = r.u32();
          if (!flags)
            break;
          require(o.integers.size() / 2 < options.max_objects,
                  "node override count limit");
          o.integers.push_back(flags);
          o.integers.push_back(r.u32());
        }
      } else if (element == "LcOpShadFragMaterialElement") {
        auto n = count();
        o.integers.push_back(n);
        for (uint32_t j = 0; j < n; ++j) {
          auto appearance = ref();
          require(appearance.object != none &&
                      graph.objects[appearance.object].type == 55,
                  "saved material override appearance type");
          o.references.push_back(appearance);
          auto paths = count();
          o.integers.push_back(paths);
          for (uint32_t k = 0; k < paths; ++k)
            o.integers.push_back(r.u32());
        }
        if (version >= 438)
          o.references.push_back(ref());
      } else
        throw UnsupportedLayout("element record copy " + element);
      break;
    }
    case 54:
      for (unsigned i = 0; i < 14; ++i)
        o.numbers.push_back(r.f32());
      break;
    case 55: {
      auto a = read_appearance(r, [&](uint32_t type) {
        Id child = object();
        require(child != none && graph.objects[child].type == type,
                "appearance child type");
        return child;
      });
      o.flags = a.flags;
      o.references = {{graph_id, a.material}, {graph_id, a.asset}};
      o.integers.assign(a.overrides.begin(), a.overrides.end());
      break;
    }
    case 52:
    case 82:
      o.strings.push_back(str());
      o.strings.push_back(str());
      break;
    case 53:
    case 25:
    case 22:
    case 32: {
      if (o.type == 32)
        o.integers.push_back(version >= 203 ? r.u32() : 0);
      common(o);
      o.integers.push_back(version >= 215 ? r.u32() : 0);
      auto n = count();
      o.attributes.reserve(n);
      for (uint32_t j = 0; j < n; ++j)
        o.attributes.push_back(ref());
      if (o.type != 22 && !(root_partition && id == 100 && o.type == 32)) {
        n = count();
        o.children.reserve(n);
        for (uint32_t j = 0; j < n; ++j)
          o.children.push_back(object());
      }
      if (o.type == 32) {
        o.integers.push_back(r.u32());
        n = count();
        for (uint32_t j = 0; j < n; ++j)
          o.references.push_back(ref());
        doubles(o, 3);
        o.integers.push_back(r.u32());
        o.strings.push_back(str());
        o.integers.push_back(r.u32());
        o.integers.push_back(r.u32());
        o.integers.push_back(r.u32());
        doubles(o, 6);
        o.integers.push_back(r.u32());
        o.strings.push_back(str());
        o.integers.push_back(r.u32());
        o.strings.push_back(str());
        if (version >= 109)
          doubles(o, 3);
        if (version >= 230)
          doubles(o, 4);
        if (version >= 421) {
          auto has = r.u32();
          o.integers.push_back(has);
          if (has) {
            auto schemas = count();
            o.integers.push_back(schemas);
            for (uint32_t j = 0; j < schemas; ++j)
              o.integers.push_back(r.u32());
          }
        }
      }
      break;
    }
    case 31:
      common(o);
      doubles(o, 14);
      break;
    case 84:
    case 86: {
      common(o);
      uint32_t n = count();
      o.properties.reserve(n);
      for (uint32_t j = 0; j < n; ++j) {
        Property p;
        p.name = object();
        p.value = value();
        o.properties.push_back(p);
      }
      break;
    }
    case 40:
    case 41:
      common(o);
      doubles(o, o.type == 40 ? 3 : 7);
      break;
    case 39:
      common(o);
      doubles(o, 12);
      o.integers.push_back(r.u32());
      break;
    case 38:
    case 68:
    case 70:
      common(o);
      break;
    case 102:
      o.strings.push_back(str());
      doubles(o, 1);
      for (unsigned j = 0; j < 4; ++j)
        o.integers.push_back(r.u32());
      break;
    case 74:
    case 76:
      common(o);
      o.strings.push_back(str());
      break;
    case 64:
    case 66:
    case 67:
      common(o);
      o.wide.push_back(r.u64());
      break;
    case 92: {
      common(o);
      validate_publish_attribute_flags(o.flags, version);
      o.integers.push_back(0);
      read_publish_body(
          r, o.integers.back(), [&](unsigned) { o.strings.push_back(str()); },
          [&](unsigned, int64_t time) {
            o.wide.push_back(static_cast<uint64_t>(time));
          });
      break;
    }
    case 77:
    case 79: {
      common(o);
      uint32_t n = count();
      o.integers.push_back(n);
      for (uint32_t j = 0; j < n; ++j) {
        o.strings.push_back(str());
        o.strings.push_back(str());
        o.references.push_back(ref());
        uint32_t points = count();
        o.integers.push_back(points);
        doubles(o, points * 3);
      }
      break;
    }
    case 200:
      common(o);
      o.references.push_back(ref());
      break;
    case 97:
      common(o);
      for (uint32_t j = 0; j < 5; ++j) {
        auto has = r.u32();
        require(has <= 1, "shader present flag");
        if (has) {
          auto s = shader();
          s.slot = j;
          o.shaders.push_back(std::move(s));
        }
      }
      break;
    case 98:
      common(o);
      o.flags = r.u32();
      require(o.flags <= 5, "texture space enum");
      if (o.flags >= 1 && o.flags <= 3)
        o.shaders.push_back(shader());
      break;
    case 182: {
      o.strings.push_back(str());
      o.strings.push_back(str());
      auto n = count();
      for (uint32_t j = 0; j < n; ++j)
        o.protein_properties.push_back(protein(index));
      if (version >= 254)
        o.references.push_back(ref());
      if (version >= 411) {
        r.align(4);
        auto b = r.raw(16);
        o.bytes.assign(b.begin(), b.end());
      }
      if (version >= 415)
        for (unsigned k = 0; k < 3; ++k) {
          n = count();
          o.integers.push_back(n);
          for (uint32_t j = 0; j < n; ++j)
            o.strings.push_back(str());
        }
      if (version >= 417)
        o.strings.push_back(str());
      if (version >= 429)
        o.strings.push_back(str());
      if (version >= 430)
        o.strings.push_back(str());
      break;
    }
    case 183: {
      o.strings.push_back(str());
      o.strings.push_back(str());
      auto n = count();
      auto b = r.raw(n);
      o.bytes.assign(b.begin(), b.end());
      o.references.push_back(ref());
      break;
    }
    case 185: {
      o.strings.push_back(str());
      if (version >= 447)
        o.strings.push_back(str());
      auto n = count();
      o.integers.push_back(n);
      for (uint32_t j = 0; j < n; ++j) {
        o.strings.push_back(str());
        auto embedded = r.u32();
        require(embedded <= 1, "invalid asset embedded flag");
        o.integers.push_back(embedded);
        if (embedded) {
          auto name = o.strings.back();
          graph.embedded_files.push_back(read_embedded_asset(
              r, name == none ? std::string{} : graph.strings[name], index, j));
        } else
          o.strings.push_back(str());
      }
      break;
    }
    case 27:
      o.integers.push_back(r.u32());
      if (version >= 252)
        o.references.push_back(ref());
      o.flags = r.u32();
      doubles(o, 12);
      break;
    default:
      r.fail("unsupported metadata type " + std::to_string(o.type));
    }
    graph.objects[index] = std::move(o);
    return index;
  }
  void partition() {
    graph.roots.push_back(object());
    r.exact();
  }
  Id external(Cursor &input, bool is_string) {
    r.pos = input.pos;
    auto id = is_string ? str() : object();
    input.pos = r.pos;
    return id;
  }
  std::vector<uint32_t> hierarchy() {
    auto n = count();
    std::vector<uint32_t> pages(n);
    for (auto &v : pages)
      v = count();
    auto rootid = r.u32();
    require(rootid >= 100 && rootid < options.max_objects,
            "invalid hierarchy root");
    ids.resize(rootid + 1, none);
    ids[rootid] = 0;
    graph.objects.emplace_back();
    graph.objects[0].type = 32;
    n = count();
    for (uint32_t j = 0; j < n; ++j)
      graph.roots.push_back(object());
    require(r.u32() == 1, "unsupported run path layout");
    r.exact();
    return pages;
  }
  void properties(uint32_t n) {
    graph.roots.reserve(n);
    for (uint32_t j = 0; j < n; ++j)
      graph.roots.push_back(object());
    r.exact();
  }
  void shared() {
    auto n = count();
    properties(n);
  }
};
struct ObjectReader::Impl {
  Options options;
  GraphReader reader;
  Impl(std::span<const uint8_t> b, ObjectGraph &g, uint32_t v, Options o)
      : options(std::move(o)), reader(b, g, 0, v, options) {}
};
ObjectReader::ObjectReader(std::span<const uint8_t> b, ObjectGraph &g,
                           uint32_t v, Options options)
    : impl(std::make_unique<Impl>(b, g, v, std::move(options))) {}
ObjectReader::~ObjectReader() = default;
Id ObjectReader::object(Cursor &r) { return impl->reader.external(r, false); }
Id ObjectReader::string(Cursor &r) { return impl->reader.external(r, true); }
void read_partition(Model &model, std::span<const uint8_t> part,
                    uint32_t version, const Options &options) {
  model.graphs.resize(2);
  model.schema_references.clear();
  GraphReader(part, model.graphs[0], 0, version, options, true).partition();
  auto &partition = model.graphs[0].objects[model.graphs[0].roots[0]];
  require(partition.integers.size() >= 7, "incomplete partition header");
  model.linear_units = static_cast<int>(partition.integers[5]);
  switch (model.linear_units) {
  case 0:
    model.meters_per_unit = 1;
    break;
  case 1:
    model.meters_per_unit = .01;
    break;
  case 2:
    model.meters_per_unit = .001;
    break;
  case 3:
    model.meters_per_unit = .3048;
    break;
  case 4:
    model.meters_per_unit = .0254;
    break;
  case 5:
    model.meters_per_unit = .9144;
    break;
  case 6:
    model.meters_per_unit = 1000;
    break;
  case 7:
    model.meters_per_unit = 1609.344;
    break;
  case 8:
    model.meters_per_unit = .000001;
    break;
  case 9:
    model.meters_per_unit = .0000254;
    break;
  case 10:
    model.meters_per_unit = .0000000254;
    break;
  default:
    break;
  }
  if (version >= 421 && partition.integers.size() > 10 &&
      partition.integers[9]) {
    for (size_t i = 11; i < partition.integers.size(); ++i) {
      auto id = partition.integers[i];
      model.schema_references.push_back(id ? id - 1 : none);
    }
  }
}
void read_metadata(Model &model, std::span<const uint8_t> file,
                   const std::vector<Chunk> &chunks, uint32_t version,
                   const Options &options, std::vector<bool> &parsed) {
  auto find = [&](const std::string &suffix) -> const Chunk & {
    for (auto &c : chunks)
      if (c.name == (model.name.empty() ? "" : model.name + "\\") + suffix) {
        parsed[static_cast<size_t>(&c - chunks.data())] = true;
        return c;
      }
    throw Error("missing metadata chunk " + suffix);
  };
  auto raw = [&](const std::string &suffix) {
    auto blocks = chunk_blocks(file, find(suffix), options);
    if (blocks.size() == 1)
      return std::move(blocks.front());
    Bytes data;
    size_t size = 0;
    for (auto &b : blocks)
      size += b.size();
    data.reserve(size);
    for (auto &b : blocks)
      data.insert(data.end(), b.begin(), b.end());
    return data;
  };
  auto shared = raw("LcOpNwdSharedNodes");
  GraphReader(shared, model.shared_nodes, none, version, options).shared();
  for (const auto &geometry : model.geometries)
    if (geometry.type == 103)
      require(geometry.text_style < model.shared_nodes.roots.size(),
              "text style outside shared nodes");
  model.graphs.resize(2);
  auto part = raw("LcOpNwdPartition");
  read_partition(model, part, version, options);
  auto hierarchy = raw("LcOpNwdLogicalHierarchy");
  auto counts =
      GraphReader(hierarchy, model.graphs[1], 1, version, options).hierarchy();
  if (!counts.empty()) {
    auto &chunk = find("LcOaPartitionProps");
    require(chunk.index_bytes && chunk.prefix_bytes,
            "missing property block index");
    auto index =
        inflate_one(file.subspan(chunk.offset + chunk.size - chunk.index_bytes,
                                 chunk.index_bytes),
                    options.max_decoded_chunk);
    Cursor r(index.bytes, "property page index");
    require(r.u32() == counts.size(), "property page count mismatch");
    std::vector<uint32_t> sizes(counts.size());
    uint64_t total = 0;
    for (auto &size : sizes) {
      size = r.u32();
      total += size;
    }
    r.exact();
    require(total + chunk.prefix_bytes + chunk.index_bytes == chunk.size,
            "property compressed lengths mismatch");
    std::vector<uint64_t> offsets(sizes.size() + 1);
    offsets[0] = chunk.offset + chunk.prefix_bytes;
    for (size_t i = 0; i < sizes.size(); ++i)
      offsets[i + 1] = offsets[i] + sizes[i];
    model.graphs.resize(2 + counts.size());
    parallel_for(counts.size(), options.threads, [&](size_t i) {
      auto block = inflate_one(file.subspan(offsets[i], sizes[i]),
                               options.max_decoded_chunk);
      require(block.consumed == sizes[i], "property block compressed boundary");
      GraphReader(block.bytes, model.graphs[2 + i], static_cast<Id>(2 + i),
                  version, options)
          .properties(counts[i]);
    });
  }
  if (counts.empty()) {
    const auto name =
        (model.name.empty() ? "" : model.name + "\\") + "LcOaPartitionProps";
    for (size_t i = 0; i < chunks.size(); ++i) {
      const auto &c = chunks[i];
      if (c.name != name)
        continue;
      require(c.flags == 1 && c.prefix_bytes == 4 && c.index_bytes &&
                  c.prefix_bytes + c.index_bytes == c.size,
              "empty property page envelope");
      Cursor prefix(file.subspan(c.offset, c.prefix_bytes));
      require(prefix.u32() == 0, "empty property page prefix");
      auto index =
          inflate_one(file.subspan(c.offset + c.prefix_bytes, c.index_bytes),
                      options.max_decoded_chunk);
      require(index.consumed == c.index_bytes, "empty property index extent");
      Cursor r(index.bytes);
      require(r.u32() == 0,
              "unexpected property pages without hierarchy references");
      r.exact();
      parsed[i] = true;
    }
  }
  auto &graph = model.graphs[1];
  size_t page = 2, slot = 0;
  std::vector<Id> order(graph.objects.size());
  for (size_t i = 0; i < order.size(); ++i)
    order[i] = static_cast<Id>(i);
  std::stable_sort(order.begin(), order.end(), [&](Id a, Id b) {
    return graph.objects[a].stream_offset < graph.objects[b].stream_offset;
  });
  for (Id id : order)
    for (auto &a : graph.objects[id].attributes)
      if (a.object == none) {
        while (page < model.graphs.size() &&
               slot == model.graphs[page].roots.size()) {
          ++page;
          slot = 0;
        }
        require(page < model.graphs.size(),
                "more attribute placeholders than property records");
        a = {static_cast<Id>(page), model.graphs[page].roots[slot++]};
        ++model.property_attribute_count;
      }
  uint64_t expected = 0;
  for (auto n : counts)
    expected += n;
  require(expected == model.property_attribute_count,
          "unbound property page records");
  model.paths.push_back({none, 0});
  std::vector<bool> active(graph.objects.size());
  std::function<void(Id, Id, unsigned)> walk = [&](Id id, Id parent,
                                                   unsigned depth) {
    require(depth < 512 && id < graph.objects.size() && !active[id],
            "invalid/cyclic hierarchy");
    require(model.paths.size() < options.max_objects,
            "expanded path count limit");
    active[id] = true;
    Id path = static_cast<Id>(model.paths.size());
    model.paths.push_back({parent, id});
    for (Id child : graph.objects[id].children)
      walk(child, path, depth + 1);
    active[id] = false;
  };
  for (Id id : graph.roots)
    walk(id, 0, 0);
}
} // namespace nwd::detail
