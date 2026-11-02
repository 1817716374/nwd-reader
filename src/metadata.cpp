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
  size_t spatial_nodes = 0;
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
      common_light(o);
      doubles(o, 12);
      break;
    case 28:
      if (version != 103)
        r.fail("metadata type 28 layout unverified for this container version");
      common_light(o);
      doubles(o, 16);
      break;
    case 29:
      if (version != 103)
        r.fail("metadata type 29 layout unverified for this container version");
      common_light(o);
      doubles(o, 21);
      break;    default:
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
    bind_inline_partition(model, options, version);
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
  auto raw_chunk = [&](const Chunk &chunk) {
    auto blocks = chunk_blocks(file, chunk, options);
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
  auto raw = [&](const std::string &suffix) { return raw_chunk(find(suffix)); };
  const auto shared_name =
      (model.name.empty() ? "" : model.name + "\\") +
      "LcOpNwdSharedNodes";
  const Chunk *shared_chunk = nullptr;
  for (const auto &chunk : chunks)
    if (chunk.name == shared_name) {
      require(shared_chunk == nullptr, "duplicate shared-nodes chunk");
      shared_chunk = &chunk;
    }
  if (shared_chunk) {
    parsed[static_cast<size_t>(shared_chunk - chunks.data())] = true;
    auto shared_context = context ? *context : GeometryStreamContext{};
    shared_context.paged = true; // modern model entry is anchored by Geometry
    shared_context.schemas = schemas;
    read_shared_nodes(model, raw_chunk(*shared_chunk), version, options,
                      &shared_context);
  }
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
    const auto &children = graph.objects[id].children;
    if (version < 37) {
      for (auto child = children.rbegin(); child != children.rend(); ++child)
        walk(*child, path, depth + 1);
    } else {
      for (Id child : children)
        walk(child, path, depth + 1);
    }
    active[id] = false;
  };
  if (version < 37) {
    for (auto id = graph.roots.rbegin(); id != graph.roots.rend(); ++id)
      walk(*id, 0, 0);
  } else {
    for (Id id : graph.roots)
      walk(id, 0, 0);
  }
}
} // namespace nwd::detail

namespace nwd {
std::string_view metadata_type_name(uint32_t type,
                                   uint32_t container_version) noexcept {
  static constexpr std::array<std::string_view, 184> names{
      "LcOaBad",
      "LcOa_LcOcStaticGroup",
      "LcOa_LcOcStaticLeaf",
      "LcOa_csMaterial",
      "LcOa_csAppearance",
      "LcOa_csCoordSet3f",
      "LcOa_csNormalSet3f",
      "LcOa_csNormalSet3s",
      "LcOa_csColorSet3f",
      "LcOa_csColorSet4f",
      "LcOa_csTexCoordSet2f",
      "LcOa_csIndexSet",
      "LcOa_csTriStripSet",
      "LcOaCoreShape",
      "LcOaRunTranslation",
      "LcOaRunTransRot",
      "LcOaRunTransRotScale",
      "LcOaRunMatrix",
      "LcOaNodeModified",
      "LcOaGroupModified",
      "LcOaUnknownAttribute",
      "LcOaUnknownNode",
      "LcOaExGeometry",
      "LcOaUnknownGeometry",
      "LcOaUnknownGraph",
      "LcOaGroup",
      "LcOaUnknownLight",
      "LcOaDistantLight",
      "LcOaPointLight",
      "LcOaSpotLight",
      "LcOaUnknownMaterial",
      "LcOaExMaterial",
      "LcOaPartition",
      "LcOaSemanticPriority",
      "LcOaUnknownSemanticPriority",
      "LcOaExSemanticPriority",
      "LcOaTransform",
      "LcOaUnknownTransform",
      "LcOaExBaseTransform",
      "LcOaExTransform",
      "LcOaExTrans",
      "LcOaExTransRot",
      "LcOaUnknownShape",
      "LcOaSensor",
      "LcOaPath",
      "LcOpCsbMaterial",
      "LcOpCsbGeometry",
      "LcOpCsbTransform",
      "LcOlLWGeometry",
      "LcOlLWMaterial",
      "LcOlLWTransform",
      "LcOlLWGraph",
      "LcOaRefMultiString",
      "LcOaExGroup",
      "LcOglMaterial",
      "LcOglAppearance",
      "LcOglColorSet3f",
      "LcOglColorSet4f",
      "LcOglColorSet4ub",
      "LcOglNormalSet3f",
      "LcOglCoordSet3f",
      "LcOglTexCoordSet2f",
      "LcOglTriStripSet",
      "LcOaPassiveAttribute",
      "LcOaNat64Attribute",
      "LcOaUnknownNat64Attribute",
      "LcOaExNat64Attribute",
      "LcOaExAnonNat64Attribute",
      "LcOaNameAttribute",
      "LcOaUnknownNameAttribute",
      "LcOaExNameAttribute",
      "LcOaExGeometryAttribute",
      "LcOaRunDynamicTransform",
      "LcOaRunPath",
      "LcOaTextAttribute",
      "LcOaUnknownTextAttribute",
      "LcOaExTextAttribute",
      "LcOaURLAttribute",
      "LcOaUnknownURLAttribute",
      "LcOaExURLAttribute",
      "LcOglPointSet",
      "LcOglLineStripSet",
      "LcUName",
      "LcUData",
      "LcOaPropertyAttribute",
      "LcOaUnknownPropertyAttribute",
      "LcOaExPropertyAttribute",
      "LcOglCacheNode",
      "LcOaBinaryAttribute",
      "LcOpElementRecord",
      "LcOaPublishAttribute",
      "LcOaUnknownPublishAttribute",
      "LcOaExPublishAttribute",
      "LcOglGeomRef",
      "LcOglTriStripSet4",
      "LcOglLineStripSet4",
      "LcOglPointSet4",
      "LcOaPresenterMaterialAttribute",
      "LcOaPresenterTextureSpaceAttribute",
      "LcOaMultiGeometry",
      "LcOglNormalSet3s",
      "LcOglBillboard",
      "LcOglTextStyle",
      "LcOglTextNode",
      "LcOglParametricCircle",
      "LcOglParametricCylinder",
      "LcOaPlotGeometryAttribute",
      "LcOaPlotPaperAttribute",
      "LcOaPlotEmbeddedFontsAttribute",
      "LcPlTransform",
      "LcPlAppearance",
      "LcPlArcSegment",
      "LcPlCanvas",
      "LcPlCanvasStyle",
      "LcPlCircleSegment",
      "LcPlColor",
      "LcPlEllipseSegment",
      "LcPlEmbeddedFont",
      "LcPlFontStyle",
      "LcPlGeometryBrush",
      "LcPlGeometryStream",
      "LcPlHatchBrush",
      "LcPlImageBrush",
      "LcPlImageHeader",
      "LcPlLinearGradientBrush",
      "LcPlPaperStyle",
      "LcPlPath",
      "LcPlPathFigure",
      "LcPlPathFigureStyle",
      "LcPlPathStreamStyle",
      "LcPlPathStyle",
      "LcPlSegmentOfPoints",
      "LcPlPointListSegment",
      "LcPlPolyBezierSegment",
      "LcPlPolyLineSegment",
      "LcPlPolyQuadraticBezierSegment",
      "LcPlPolyTriangle",
      "LcPlRadialGradientBrush",
      "LcPlResource",
      "LcPlSolidColorBrush",
      "LcPlStream",
      "LcPlStreamEntry",
      "LcPlStreamEntryContainer",
      "LcPlStroke",
      "LcPlText",
      "LcPlTextLine",
      "LcPlTextStyle",
      "LcPlTransform",
      "LcPlVertexColorBrush",
      "LcPlViewport",
      "LcAnActionShowViewpoint",
      "LcAnActionSetVariable",
      "LcAnActionPause",
      "LcAnActionPlayAnimation",
      "LcAnActionStopAnimation",
      "LcAnActionMessage",
      "LcAnActionLoadModel",
      "LcAnActionStoreProperty",
      "LcAnEventOnKeyPress",
      "LcAnHotspotSphere",
      "LcAnHotspotSelectionSphere",
      "LcAnEventOnHotspot",
      "LcAnEventOnVariable",
      "LcAnEventOnScript",
      "LcAnEventOnTimer",
      "LcAnEventOnAnimation",
      "LcAnEventOnCollision",
      "LcAnEventOnStart",
      "LcPlViewportStyle",
      "LcPlCoreShape",
      "LcPlGeomRef",
      "LcPlCoreShapeRepresentation",
      "LcPlCoreShapePathFigure",
      "LcPlPathFigure",
      "LcPlSegment",
      "LcPlCoreShapeText",
      "LcPlPathFigureAppearance",
      "LcPlTextAppearance",
      "LcPlBrush",
      "LcPlHatchPattern",
      "LcOaGuidAttribute",
      "LcOglExternalGeometry",
      "LcOglProteinAsset",
      "LcOglProteinAssetSchema",
  };
  if (container_version == 103)
    return type < names.size() ? names[type] : std::string_view{};
  // Indices 0..44 match the Freedom 2013, Manage 2017 and Manage 2026
  // registrations. The newer containers have only been verified through 44.
  if ((container_version == 0 || container_version == 431 ||
       container_version == 448) && type < 45)
    return names[type];
  return {};
}

std::optional<MetadataPublishFields>
metadata_publish_fields(const Object &object,
                        uint32_t container_version) noexcept {
  if (container_version != 103 || object.type != 92 ||
      object.strings.size() != 8 || object.integers.size() != 1 ||
      object.wide.size() != 2)
    return std::nullopt;
  constexpr uint32_t has_password = 1u << 0;
  constexpr uint32_t display_on_open = 1u << 1;
  constexpr uint32_t has_expiry_date = 1u << 4;
  constexpr uint32_t has_been_resaved = 1u << 5;
  constexpr uint32_t allow_resave = 1u << 6;
  constexpr uint32_t display_at_password = 1u << 7;
  constexpr uint32_t embed_textures = 1u << 8;
  constexpr uint32_t embed_database_properties = 1u << 9;
  constexpr uint32_t prevent_object_property_export = 1u << 10;
  constexpr uint32_t known_flags = has_password | display_on_open |
                                   has_expiry_date | has_been_resaved |
                                   allow_resave | display_at_password |
                                   embed_textures |
                                   embed_database_properties |
                                   prevent_object_property_export;
  MetadataPublishFields result;
  result.flags = object.integers[0];
  result.unknown_flags = result.flags & ~known_flags;
  result.title = object.strings[0];
  result.author = object.strings[1];
  result.subject = object.strings[2];
  result.publisher = object.strings[3];
  result.publish_date = object.wide[0];
  result.expiry_date = object.wide[1];
  result.copyright = object.strings[4];
  result.published_for = object.strings[5];
  result.comments = object.strings[6];
  result.keywords = object.strings[7];
  result.has_password = (result.flags & has_password) != 0;
  result.display_on_open = (result.flags & display_on_open) != 0;
  result.has_expiry_date = (result.flags & has_expiry_date) != 0;
  result.has_been_resaved = (result.flags & has_been_resaved) != 0;
  result.allow_resave = (result.flags & allow_resave) != 0;
  result.display_at_password = (result.flags & display_at_password) != 0;
  result.embed_textures = (result.flags & embed_textures) != 0;
  result.embed_database_properties =
      (result.flags & embed_database_properties) != 0;
  result.prevent_object_property_export =
      (result.flags & prevent_object_property_export) != 0;
  return result;
}

std::optional<MetadataDistantLightFields>
metadata_distant_light_fields(const Object &object) noexcept {
  if (object.type != 27 || object.numbers.size() != 12)
    return std::nullopt;
  MetadataDistantLightFields result;
  for (size_t i = 0; i < 3; ++i) {
    result.ambient_color[i] = object.numbers[i];
    result.diffuse_color[i] = object.numbers[i + 3];
    result.specular_color[i] = object.numbers[i + 6];
    result.direction[i] = object.numbers[i + 9];
  }
  return result;
}

std::optional<MetadataLightStateFields>
metadata_light_state_fields(const Object &object) noexcept {
  const bool supported_layout =
      object.type == 27 ||
      (object.type == 28 && object.numbers.size() == 16) ||
      (object.type == 29 && object.numbers.size() == 21);
  if (!supported_layout || object.integers.empty())
    return std::nullopt;
  MetadataLightStateFields result;
  result.on_value = object.integers.front();
  const uint32_t state = object.flags;
  result.state_word = state;
  result.on = result.on_value != 0;
  result.eye = (state & 1u) != 0;
  result.cast_shadow = (state & 2u) != 0;
  result.unknown_state_bits = state & ~3u;
  return result;
}

std::optional<MetadataMaterialFields>
metadata_material_fields(const Object &object) noexcept {
  if (object.type != 31 || object.numbers.size() != 14)
    return std::nullopt;
  MetadataMaterialFields result;
  for (size_t i = 0; i < 3; ++i) {
    result.ambient_color[i] = object.numbers[i];
    result.diffuse_color[i] = object.numbers[i + 3];
    result.specular_color[i] = object.numbers[i + 6];
    result.emissive_color[i] = object.numbers[i + 9];
  }
  result.shininess = object.numbers[12];
  result.transparency = object.numbers[13];
  return result;
}

std::optional<Reference>
metadata_protein_material_asset_reference(const Object &object) noexcept {
  if (object.type != 200 || object.references.size() != 1)
    return std::nullopt;
  return object.references.front();
}

std::vector<MetadataProteinMaterialAssetCandidates>
metadata_protein_material_asset_candidates(const Model &model) {
  std::unordered_map<std::string_view, std::vector<Id>> assets_by_json;
  assets_by_json.reserve(model.assets.size());
  std::vector<std::unordered_map<Id, std::vector<Id>>> assets_by_graph_object(
      model.graphs.size());
  for (Id asset = 0; asset < model.assets.size(); ++asset)
    if (!model.assets[asset].json.empty())
      assets_by_json[model.assets[asset].json].push_back(asset);
    else if (model.assets[asset].protein &&
             model.assets[asset].protein->graph < model.graphs.size() &&
             model.assets[asset].protein->object != none)
      assets_by_graph_object[model.assets[asset].protein->graph]
          [model.assets[asset].protein->object]
              .push_back(asset);

  std::vector<MetadataProteinMaterialAssetCandidates> result;
  std::vector<std::unordered_map<Id, size_t>> candidates_by_graph(
      model.graphs.size());
  for (Id graph_id = 0; graph_id < model.graphs.size(); ++graph_id) {
    const auto &graph = model.graphs[graph_id];
    for (Id object_id = 0; object_id < graph.objects.size(); ++object_id) {
      const auto &object = graph.objects[object_id];
      const auto reference = metadata_protein_material_asset_reference(object);
      if (!reference || reference->object == none ||
          reference->graph >= model.graphs.size())
        continue;
      const auto &target_graph = model.graphs[reference->graph];
      if (reference->object >= target_graph.objects.size())
        continue;
      const auto &target = target_graph.objects[reference->object];
      MetadataProteinMaterialAssetCandidates candidates;
      candidates.attribute = {graph_id, object_id};
      candidates.serialized_asset = *reference;
      if (target.type == 182) {
        candidates.match_kind =
            MetadataProteinMaterialAssetMatchKind::graph_object_identity;
        const auto &graph_assets = assets_by_graph_object[reference->graph];
        const auto match = graph_assets.find(reference->object);
        if (match != graph_assets.end())
          candidates.assets = match->second;
      } else if (target.type == 185 && !target.strings.empty()) {
        candidates.match_kind =
            MetadataProteinMaterialAssetMatchKind::exact_json_content;
        const auto match = assets_by_json.find(
            resolve_string(target_graph, target.strings.front()));
        if (match != assets_by_json.end())
          candidates.assets = match->second;
      } else {
        continue;
      }
      candidates_by_graph[graph_id].emplace(object_id, result.size());
      result.push_back(std::move(candidates));
    }
  }

  for (Id graph_id = 0; graph_id < model.graphs.size(); ++graph_id) {
    const auto &graph = model.graphs[graph_id];
    for (Id owner_id = 0; owner_id < graph.objects.size(); ++owner_id) {
      const auto &owner = graph.objects[owner_id];
      for (Id ordinal = 0; ordinal < owner.attributes.size(); ++ordinal) {
        const auto attribute = owner.attributes[ordinal];
        if (attribute.graph >= candidates_by_graph.size())
          continue;
        const auto found = candidates_by_graph[attribute.graph].find(
            attribute.object);
        if (found == candidates_by_graph[attribute.graph].end())
          continue;
        MetadataProteinMaterialAssetCandidates::Owner binding;
        binding.object = {graph_id, owner_id};
        binding.attribute_ordinal = ordinal;
        result[found->second].owners.push_back(std::move(binding));
      }
    }
  }

  if (model.hierarchy_graph < model.graphs.size()) {
    std::unordered_map<Id, std::vector<std::pair<size_t, size_t>>> owner_paths;
    for (size_t candidate = 0; candidate < result.size(); ++candidate)
      for (size_t owner = 0; owner < result[candidate].owners.size(); ++owner) {
        const auto &reference = result[candidate].owners[owner].object;
        if (reference.graph == model.hierarchy_graph)
          owner_paths[reference.object].emplace_back(candidate, owner);
      }
    for (Id path = 0; path < model.paths.size(); ++path) {
      const auto found = owner_paths.find(model.paths[path].object);
      if (found == owner_paths.end())
        continue;
      for (const auto &[candidate, owner] : found->second)
        result[candidate].owners[owner].paths.push_back(path);
    }
  }
  return result;
}

std::optional<MetadataLightFields>
metadata_light_fields(const Object &object) noexcept {
  const auto &v = object.numbers;
  if (object.type != 28 && object.type != 29)
    return std::nullopt;
  if (object.type == 28 && v.size() != 15 && v.size() != 16)
    return std::nullopt;
  if (object.type == 29 && v.size() != 20 && v.size() != 21)
    return std::nullopt;

  MetadataLightFields result;
  auto copy3 = [&](size_t at, std::array<double, 3> &out) {
    for (size_t i = 0; i < out.size(); ++i)
      out[i] = v[at + i];
  };
  copy3(0, result.ambient_color);
  copy3(3, result.diffuse_color);
  copy3(6, result.specular_color);
  copy3(9, result.position);
  if (object.type == 28) {
    copy3(12, result.attenuation);
    if (v.size() == 16)
      result.radius_of_influence = v[15];
    return result;
  }

  if (v.size() == 20) {
    copy3(12, result.direction.emplace());
    copy3(15, result.attenuation);
    result.drop_off_rate = v[18];
    result.cutoff_angle = v[19];
  } else {
    copy3(12, result.attenuation);
    result.radius_of_influence = v[15];
    copy3(16, result.direction.emplace());
    result.drop_off_rate = v[19];
    result.cutoff_angle = v[20];
  }
  return result;
}
} // namespace nwd
