#include "internal.hpp"
#include "geometry_stream.hpp"
#include "shape_fields.hpp"
#include "legacy_cs.hpp"
namespace nwd::detail {
class GraphReader {
  Cursor r;
  ObjectGraph &graph;
  Id graph_id;
  uint32_t version;
  const Options &options;
  bool root_partition;
  std::optional<GeometryStreamContext> geometry_context;
  struct ActiveNode {
    Id id;
    ActiveNode *previous;
  };
  ActiveNode *active_node = nullptr;
  std::vector<Id> ids;
  std::vector<std::pair<uint32_t, Id>> *wire_objects = nullptr;
  std::unordered_map<std::string, Id> strings;
  unsigned depth = 0;
  GeometryObjectArena &geometry_arena() {
    if (!geometry_context)
      throw UnsupportedLayout(
          "geometry object requires explicit stream context");
    if (!graph.geometry_arena)
      graph.geometry_arena = std::make_shared<GeometryObjectArena>();
    return *graph.geometry_arena;
  }
  template <class T>
  const T &geometry_owner(const std::vector<T> &table, Id id) {
    auto it = std::lower_bound(
        table.begin(), table.end(), id,
        [](const T &value, Id owner) { return value.owner < owner; });
    require(it != table.end() && it->owner == id,
            "geometry object has no completed payload");
    return *it;
  }
  uint32_t count() {
    auto n = r.u32();
    require(n <= options.max_objects, "metadata count resource limit");
    return n;
  }
  void validate_primitive_attributes(const GeometryPrimitiveObject &value,
                                     bool legacy = false) {
    auto &arena = *graph.geometry_arena;
    for (unsigned slot = 0; slot < 3; ++slot) {
      const auto id = value.attributes[slot];
      if (id == none)
        continue;
      const auto &a = geometry_owner(arena.attributes, id).data;
      require(legacy || graph.objects[id].type == a.type,
              "legacy array in a modern checked geometry reference");
      require(slot == 0   ? a.type == 61
              : slot == 1 ? a.type == 59 || a.type == 100
                          : a.type >= 56 && a.type <= 58,
              "primitive attribute object type");
      require(a.indices.size() >= value.vertex_count,
              "primitive attribute slot mismatch");
    }
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
  Reference legacy_base_ref(bool nullable) {
    const auto result = ref();
    if (result.object == none) {
      require(nullable, "null obsolete BaseVector entry");
      return result;
    }
    for (auto node = active_node; node; node = node->previous)
      if (node->id == result.object)
        return result;
    // Confirmed LcOaBase subclass registrations. Raw GL objects, names and
    // obsolete modification records occupy the same wire table but are not
    // Base objects. Keep unverified/new class registrations explicit.
    switch (graph.objects[result.object].type) {
    case 14:
    case 15:
    case 16:
    case 17:
    case 22:
    case 25:
    case 27:
    case 28:
    case 29:
    case 31:
    case 32:
    case 33:
    case 35:
    case 36:
    case 38:
    case 39:
    case 40:
    case 41:
    case 43:
    case 53:
    case 63:
    case 64:
    case 66:
    case 67:
    case 68:
    case 70:
    case 71:
    case 72:
    case 74:
    case 76:
    case 77:
    case 79:
    case 84:
    case 86:
    case 88:
    case 92:
    case 97:
    case 98:
    case 99:
    case 106:
    case 180:
    case 184:
      return result;
    default:
      throw UnsupportedLayout(
          "unverified obsolete Base reference class " +
          std::to_string(graph.objects[result.object].type));
    }
  }
  void common(Object &o) {
    if (version >= 22) {
      o.name = str();
      o.class_name = object();
    }
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
    for (uint32_t j = 0; j < n; ++j) {
      const auto connection = ref();
      require(connection.object == none ||
                  graph.objects[connection.object].type == 182,
              "Protein connection asset type");
      p.connections.push_back(connection);
    }
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
              const Options &op, bool root = false,
              const GeometryStreamContext *context = nullptr,
              std::vector<std::pair<uint32_t, Id>> *wire = nullptr)
      : r(b, "metadata graph " + std::to_string(gid)), graph(g), graph_id(gid),
        version(ver), options(op), root_partition(root),
        geometry_context(context ? std::optional(*context) : std::nullopt),
        wire_objects(wire) {
    require(!context || context->version == version,
            "geometry context version mismatch");
  }
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
    // Publish the saved class identity before nested fields can refer back to
    // this object. Payload completion is still checked by the sparse arenas.
    graph.objects[index].type = o.type;
    if (wire_objects)
      wire_objects->emplace_back(id, index);
    switch (o.type) {
    case 3: {
      auto &arena = geometry_arena();
      arena.legacy_materials.push_back(read_legacy_material(r, index));
      break;
    }
    case 4: {
      auto &arena = geometry_arena();
      const auto slot = arena.legacy_appearances.size();
      arena.legacy_appearances.emplace_back();
      LegacyAppearanceObject value;
      value.owner = index;
      const uint64_t low = r.u32(), high = r.u32();
      value.excluded_fields = low | (high << 32);
      const auto present = ~value.excluded_fields;
      if (present & ~uint64_t(0x03000600))
        throw UnsupportedLayout("legacy appearance field mask");
      for (unsigned i = 0; i < 2; ++i) {
        if (present & (uint64_t(1) << (24 + i))) {
          const auto material = object();
          require((i == 1 && material == none) ||
                      (material != none && graph.objects[material].type == 3),
                  "legacy appearance material type");
          value.materials[i] = material;
          o.references.push_back({graph_id, material});
        }
      }
      if (present & (1u << 9))
        value.saved_boolean = r.u32();
      if (present & (1u << 10))
        value.saved_enum = r.u32();
      arena.legacy_appearances[slot] = std::move(value);
      break;
    }
    case 5: {
      auto &arena = geometry_arena();
      auto value = read_coordinate_payload(
          r, true,
          std::min<uint64_t>(options.max_objects,
                             options.max_decoded_chunk / (3 * sizeof(float))));
      arena.coordinates.push_back(
          {index, std::move(value.values), std::move(value.indices), {}});
      break;
    }
    case 6:
    case 8:
    case 9:
    case 10: {
      auto &arena = geometry_arena();
      const auto type = o.type == 6   ? 59u
                        : o.type == 8 ? 56u
                        : o.type == 9 ? 57u
                                      : 61u;
      auto value = read_legacy_float_attribute(r, type, options.max_objects);
      arena.attributes.push_back({index, std::move(value)});
      break;
    }
    case 7:
    case 11:
      throw UnsupportedLayout("unresolved legacy cs array type " +
                              std::to_string(o.type));
    case 12: {
      auto &arena = geometry_arena();
      const auto slot = arena.legacy_geometries.size();
      arena.legacy_geometries.emplace_back();
      LegacyGeometryObject value;
      value.owner = index;
      value.enums[0] = r.u32();
      value.saved_integer = r.read<int32_t>();
      for (unsigned i = 1; i < 4; ++i)
        value.enums[i] = r.u32();
      for (auto &ref : value.references) {
        ref = object();
        o.references.push_back({graph_id, ref});
      }
      value.enums[4] = r.u32();
      const auto n = count();
      const auto raw = r.raw(uint64_t(n) * sizeof(int32_t));
      value.saved_lengths.resize(n);
      if (n)
        std::memcpy(value.saved_lengths.data(), raw.data(), raw.size());
      auto &primitive = value.primitive;
      primitive.owner = index;
      primitive.type = 94;
      primitive.coordinates = value.references[0];
      primitive.attributes = {value.references[3], value.references[1],
                              value.references[2]};
      uint64_t used = 0;
      primitive.lengths.reserve(n);
      for (auto saved : value.saved_lengths) {
        // Native Arrayus::SetArray(Arrayi) takes each low16 bits. Preserve
        // both.
        const auto length = static_cast<uint16_t>(saved);
        primitive.lengths.push_back(length);
        used += length;
      }
      const auto vertices =
          primitive.coordinates == none
              ? 0
              : geometry_owner(arena.coordinates, primitive.coordinates)
                    .indices.size();
      require(used <= vertices, "legacy strips exceed coordinate slots");
      primitive.vertex_count = static_cast<uint32_t>(used);
      validate_primitive_attributes(primitive, true);
      arena.legacy_geometries[slot] = std::move(value);
      break;
    }
    case 93: {
      auto &arena = geometry_arena();
      const auto slot = arena.references.size();
      arena.references.emplace_back();
      GeometryReferenceObject value;
      value.owner = index;
      value.uses_store = geometry_context->reference_uses_store();
      if (!value.uses_store) {
        value.inline_geometry = object();
        require(value.inline_geometry != none,
                "null inline geometry reference");
        const auto type = graph.objects[value.inline_geometry].type;
        if (type == 103 || type == 104 || type == 105 || type == 181) {
          require(
              geometry_owner(arena.special, value.inline_geometry).data.type ==
                  type,
              "geometry object has no completed payload");
        } else
          (void)geometry_owner(arena.primitives, value.inline_geometry);
        o.references.push_back({graph_id, value.inline_geometry});
      }
      value.user_fields_present = version >= 2;
      value.checksum_present = version >= 5;
      value.fields = read_geometry_reference_user(r, version);
      value.data_id = r.u32();
      if (value.uses_store && geometry_context->geometry_record_count) {
        require(value.data_id &&
                    value.data_id <= *geometry_context->geometry_record_count,
                "geometry reference outside source record table");
        value.store_range_checked = true;
      }
      arena.references[slot] = std::move(value);
      break;
    }
    case 14:
    case 15:
    case 16:
    case 17: {
      auto value = read_run_transform_fields(r, o.type, version);
      const auto n = o.type == 14   ? 3u
                     : o.type == 15 ? 7u
                     : o.type == 16 ? 8u
                                    : 16u;
      o.numbers.assign(value.values.begin(), value.values.begin() + n);
      break;
    }
    case 60: {
      auto &arena = geometry_arena();
      auto value = read_coordinate_payload(
          r, !(geometry_context->compression.flags & 1),
          std::min<uint64_t>(options.max_objects,
                             options.max_decoded_chunk / (3 * sizeof(float))));
      arena.coordinates.push_back({index, std::move(value.values),
                                   std::move(value.indices),
                                   std::move(value.quantization)});
      break;
    }
    case 58:
    case 59:
    case 61:
    case 100: {
      auto &arena = geometry_arena();
      const auto start = r.pos;
      if (o.type == 58)
        (void)r.u32();
      const int64_t slots = r.read<int32_t>();
      r.pos = start;
      const uint64_t n = slots < 0 ? -slots : slots;
      require(n <= options.max_objects && n <= 100000000,
              "geometry attribute slot limit");
      auto value = read_geometry_attribute_payload(
          r, o.type, static_cast<uint32_t>(n), *geometry_context,
          options.max_objects);
      arena.attributes.push_back({index, std::move(value)});
      break;
    }
    case 103: {
      auto &arena = geometry_arena();
      const auto slot = arena.special.size();
      arena.special.emplace_back(); // reserve owner order before nested styles
      arena.special[slot].owner = index;
      GeometrySpecialObject value;
      value.owner = index;
      auto &g = value.data;
      g.type = o.type;
      auto fields = read_text_parameters(r, g, version);
      fields->uses_shared_nodes = geometry_context->use_shared_nodes;
      if (fields->uses_shared_nodes) {
        fields->shared_node_index = r.read<int32_t>();
        g.text_style = static_cast<Id>(fields->shared_node_index);
      } else {
        fields->inline_style = ref();
        o.references.push_back(fields->inline_style);
      }
      g.text_fields = std::move(fields);
      arena.special[slot] = std::move(value);
      break;
    }
    case 181: {
      auto &arena = geometry_arena();
      if (version >= 434 && !geometry_context->schemas)
        throw UnsupportedLayout(
            "external geometry requires schema stream context");
      auto external = std::make_shared<ExternalGeometry>();
      read_external_header(r, *external);
      read_external_body(r, *external,
                         geometry_context->schemas.value_or(
                             std::span<const SchemaDefinition>{}),
                         version);
      const auto payload =
          r.data.subspan(external->payload_record_offset,
                         r.pos - external->payload_record_offset);
      external->unparsed_payload.assign(payload.begin(), payload.end());
      GeometrySpecialObject value;
      value.owner = index;
      value.data.type = o.type;
      value.data.external = std::move(external);
      arena.special.push_back(std::move(value));
      break;
    }
    case 104:
    case 105: {
      auto &arena = geometry_arena();
      GeometrySpecialObject value;
      value.owner = index;
      value.data.type = o.type;
      read_analytic_fields(r, value.data);
      arena.special.push_back(std::move(value));
      break;
    }
    case 94:
    case 95:
    case 96: {
      geometry_arena();
      GeometryPrimitiveObject value;
      value.owner = index;
      value.type = o.type;
      value.coordinates = object();
      require(value.coordinates == none ||
                  graph.objects[value.coordinates].type == 60,
              "primitive coordinate object type");
      o.references.push_back({graph_id, value.coordinates});
      for (auto &attribute : value.attributes) {
        attribute = object();
        o.references.push_back({graph_id, attribute});
      }
      auto &arena = *graph.geometry_arena;
      const auto n = value.coordinates == none
                         ? 0
                         : geometry_owner(arena.coordinates, value.coordinates)
                               .indices.size();
      if (o.type == 96) {
        value.vertex_count = static_cast<uint32_t>(n);
        if (version >= 14)
          value.flags = r.u32();
        require(value.flags <= 1, "point-set boolean field");
      } else {
        auto strips = read_geometry_strips(r, o.type, *geometry_context,
                                           static_cast<uint32_t>(n),
                                           options.max_objects, true);
        value.lengths = std::move(strips.lengths);
        value.indices = std::move(strips.indices);
        value.implicit_indices = strips.implicit_indices;
        uint64_t used = 0;
        for (auto length : value.lengths)
          used += length;
        if (value.implicit_indices)
          value.vertex_count = static_cast<uint32_t>(used);
        else
          for (size_t i = 0; i < used; ++i)
            value.vertex_count =
                std::max(value.vertex_count, value.indices[i] + 1);
      }
      validate_primitive_attributes(value);
      arena.primitives.push_back(std::move(value));
      break;
    }
    case 1:
    case 2: {
      if (version >= 28)
        throw UnsupportedLayout("tagged spatial children from version28");
      auto n = count();
      o.children.reserve(n);
      for (uint32_t j = 0; j < n; ++j) {
        auto child = object();
        require(child != none, "null legacy spatial child");
        const auto type = graph.objects[child].type;
        require(o.type == 2 ? type == 13 : type == 1 || type == 2 || type == 13,
                "legacy spatial child type");
        o.children.push_back(child);
      }
      break;
    }
    case 13: {
      auto &arena = geometry_arena();
      const auto slot = arena.shapes.size();
      arena.shapes.emplace_back();
      GeometryShapeObject value;
      value.owner = index;
      std::optional<GeometryLegacyShapeFields> legacy;
      const auto legacy_slot = arena.legacy_shapes.size();
      if (version == 0) {
        legacy.emplace();
        legacy->owner = index;
        arena.legacy_shapes.emplace_back();
        for (auto &v : legacy->initial_bounds)
          v = r.f64();
      }
      value.precision = r.f64();
      value.primitive_count = r.u32();
      value.bits = r.u32();
      value.flags = r.u32();
      if (version < 28) {
        value.path_object = object();
        require(value.path_object == none ||
                    graph.objects[value.path_object].type ==
                        (version == 0 ? 44u : 73u),
                "core shape path object type");
        o.references.push_back({graph_id, value.path_object});
      } else {
        value.path_encoding =
            version < 32 ? GeometryShapePath::ordinal : GeometryShapePath::link;
        value.path_reference = r.u32();
      }
      if (legacy) {
        for (auto &v : legacy->geometry_bounds)
          v = r.f64();
        legacy->geometry_tolerance = r.f64();
      }
      value.transform = object();
      require(value.transform == none ||
                  (graph.objects[value.transform].type >= 14 &&
                   graph.objects[value.transform].type <= 17),
              "core shape transform type");
      value.appearance = object();
      require(value.appearance == none ||
                  graph.objects[value.appearance].type == 55 ||
                  graph.objects[value.appearance].type == 4,
              "core shape appearance type");
      o.references.push_back({graph_id, value.transform});
      o.references.push_back({graph_id, value.appearance});
      if (version >= 243 && (value.flags & 0x20000080u) == 0x20000080u) {
        auto saved = read_auxiliary_transform_fields(r);
        value.auxiliary_transform =
            static_cast<Id>(arena.auxiliary_transforms.size());
        arena.auxiliary_transforms.push_back({index, std::move(saved)});
      }
      if (legacy) {
        legacy->geometry_present_word = r.u32();
        require(legacy->geometry_present_word != 0,
                "version0 shape requires geometry");
      }
      value.geometry_reference = object();
      require(value.geometry_reference != none &&
                  (graph.objects[value.geometry_reference].type == 93 ||
                   (version < 23 &&
                    graph.objects[value.geometry_reference].type == 12)),
              "core shape geometry reference type");
      o.references.push_back({graph_id, value.geometry_reference});
      arena.shapes[slot] = std::move(value);
      if (legacy)
        arena.legacy_shapes[legacy_slot] = std::move(*legacy);
      break;
    }
    case 44:
    case 73: {
      const auto n = count();
      o.references.reserve(n);
      for (uint32_t j = 0; j < n; ++j) {
        const auto node = ref();
        require(node.object != none, "null temporary path node");
        const auto type = graph.objects[node.object].type;
        bool valid = type == 22 || type == 25 || type == 32 || type == 53;
        // A serialized path can refer back to the partition/node currently
        // being read. Its final Object has not yet been moved into the arena.
        for (auto p = active_node; !valid && p; p = p->previous)
          valid = p->id == node.object;
        require(valid, "temporary path entry is not a logical node");
        o.references.push_back(node);
      }
      break;
    }
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
      auto a = read_appearance(r, version, [&](uint32_t type) {
        Id child = object();
        require(child == none || graph.objects[child].type == type,
                "appearance child type");
        return child;
      });
      o.flags = a.flags;
      o.references = {{graph_id, a.material}, {graph_id, a.asset}};
      o.integers.assign(a.overrides.begin(), a.overrides.end());
      if (a.legacy_discarded_enum)
        o.integers.push_back(*a.legacy_discarded_enum);
      break;
    }
    case 52:
    case 82:
      o.strings.push_back(str());
      o.strings.push_back(str());
      break;
    case 18:
      // Obsolete NodeModified keeps two Base object references. Native
      // loading discards their effect; preserve the serialized identities.
      o.references.push_back(legacy_base_ref(true));
      o.references.push_back(legacy_base_ref(true));
      break;
    case 19:
      // Two independent BaseVector payloads; null entries are invalid.
      for (unsigned list = 0; list < 2; ++list) {
        const auto n = count();
        o.integers.push_back(n);
        for (uint32_t j = 0; j < n; ++j)
          o.references.push_back(legacy_base_ref(false));
      }
      break;
    case 53:
    case 25:
    case 22:
    case 32: {
      ActiveNode current{index, active_node};
      active_node = &current;
      struct NodeScope {
        ActiveNode *&head;
        ActiveNode *previous;
        ~NodeScope() { head = previous; }
      } scope{active_node, current.previous};
      if (o.type == 32)
        o.integers.push_back(version >= 203 ? r.u32() : 0);
      common(o);
      o.integers.push_back(version >= 215 ? r.u32() : 0);
      auto n = count();
      o.attributes.reserve(n);
      for (uint32_t j = 0; j < n; ++j)
        o.attributes.push_back(ref());
      std::optional<LegacyNodeRecord> old_node;
      if (version == 0) {
        old_node.emplace();
        old_node->owner = index;
        old_node->modified = ref();
        const auto modified = old_node->modified.object;
        require(modified == none || graph.objects[modified].type == 18,
                "version0 node modified object type");
      }
      if (o.type != 22 &&
          !(root_partition && id == 100 && o.type == 32 && version >= 28)) {
        n = count();
        o.children.reserve(n);
        for (uint32_t j = 0; j < n; ++j)
          o.children.push_back(object());
        if (old_node) {
          old_node->group_modified = ref();
          const auto modified = old_node->group_modified->object;
          require(modified == none || graph.objects[modified].type == 19,
                  "version0 group modified object type");
        }
      }
      if (old_node)
        graph.legacy_nodes.push_back(std::move(*old_node));
      if (o.type == 32) {
        o.integers.push_back(r.u32());
        std::optional<LegacyPartitionRecord> legacy;
        if (version < 28) {
          legacy.emplace();
          legacy->owner = index;
          legacy->path_links_ready_offset = r.pos;
          legacy->spatial_root = ref();
          const auto spatial = legacy->spatial_root.object;
          require(spatial == none || graph.objects[spatial].type == 1 ||
                      graph.objects[spatial].type == 2,
                  "legacy partition spatial root type");
        }
        n = count();
        for (uint32_t j = 0; j < n; ++j)
          o.references.push_back(ref());
        doubles(o, 3);
        o.integers.push_back(r.u32());
        if (legacy && version >= 1) {
          legacy->shapes_present = true;
          n = count();
          legacy->shapes.reserve(n);
          for (uint32_t j = 0; j < n; ++j) {
            const auto shape = ref();
            require(shape.object == none ||
                        graph.objects[shape.object].type == 13,
                    "legacy partition shape type");
            legacy->shapes.push_back(shape);
          }
          legacy->flag = r.u32();
        }
        if (version >= 2 && version < 22) {
          legacy->legacy_names = std::array<Id, 2>{str(), str()};
          o.name = (*legacy->legacy_names)[0];
        }
        if (legacy)
          graph.legacy_partitions.push_back(std::move(*legacy));
        o.strings.push_back(version >= 3 ? str() : none);
        o.integers.push_back(version >= 3 ? r.u32() : 0);
        o.integers.push_back(version >= 5 ? r.u32() : 0);
        o.integers.push_back(version >= 5 ? r.u32() : 0);
        if (version >= 53)
          doubles(o, 6);
        o.integers.push_back(version >= 80 ? r.u32() : 0);
        o.strings.push_back(version >= 80 ? str() : none);
        o.integers.push_back(version >= 45 ? r.u32() : 0);
        o.strings.push_back(version >= 57 ? str() : none);
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
      o.numbers.push_back(r.read<double>());
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
    case 180: {
      common(o);
      require(version < 41 || !(o.flags & 0x10000),
              "invalid GUID attribute property-vector flag");
      r.align(4);
      const auto bytes = r.raw(16);
      o.bytes.assign(bytes.begin(), bytes.end());
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
      require(o.references.back().object == none ||
                  graph.objects[o.references.back().object].type ==
                      protein_asset_wire_type(version),
              "Protein material attribute asset type");
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
      if (version >= 254) {
        o.references.push_back(ref());
        require(o.references.back().object == none ||
                    graph.objects[o.references.back().object].type == 183,
                "Protein asset schema type");
      }
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
      require(o.references.back().object == none ||
                  graph.objects[o.references.back().object].type == 183,
              "Protein schema parent type");
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
  std::vector<std::pair<uint32_t, Id>> wire_objects;
  GraphReader reader;
  Impl(std::span<const uint8_t> b, ObjectGraph &g, uint32_t v, Options o,
       const GeometryStreamContext *context, bool track)
      : options(std::move(o)), reader(b, g, 0, v, options, false, context,
                                      track ? &wire_objects : nullptr) {}
};
ObjectReader::ObjectReader(std::span<const uint8_t> b, ObjectGraph &g,
                           uint32_t v, Options options,
                           const GeometryStreamContext *context, bool track)
    : impl(std::make_unique<Impl>(b, g, v, std::move(options), context,
                                  track)) {}
ObjectReader::~ObjectReader() = default;
Id ObjectReader::object(Cursor &r) { return impl->reader.external(r, false); }
Id ObjectReader::string(Cursor &r) { return impl->reader.external(r, true); }
std::span<const std::pair<uint32_t, Id>> ObjectReader::wire_objects() const {
  return impl->wire_objects;
}
void read_partition(Model &model, std::span<const uint8_t> part,
                    uint32_t version, const Options &options,
                    const GeometryStreamContext *context) {
  model.graphs.resize(2);
  model.schema_references.clear();
  GraphReader(part, model.graphs[0], 0, version, options, true, context)
      .partition();
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
  if (version < 28)
    bind_inline_partition(model, options);
}
void read_shared_nodes(Model &model, std::span<const uint8_t> bytes,
                       uint32_t version, const Options &options,
                       const GeometryStreamContext *context) {
  auto local = context ? *context : GeometryStreamContext{};
  local.version = version;
  local.use_shared_nodes = false; // enabled only after this table is read
  GraphReader(bytes, model.shared_nodes, none, version, options, false, &local)
      .shared();
}
void read_metadata(Model &model, std::span<const uint8_t> file,
                   const std::vector<Chunk> &chunks, uint32_t version,
                   const Options &options, std::vector<bool> &parsed,
                   const GeometryStreamContext *context,
                   std::span<const SchemaDefinition> schemas) {
  if (version < 28)
    throw UnsupportedLayout("inline model requires legacy container loading");
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
  auto shared_context = context ? *context : GeometryStreamContext{};
  shared_context.paged = true; // modern model entry is anchored by Geometry
  shared_context.schemas = schemas;
  read_shared_nodes(model, shared, version, options, &shared_context);
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
