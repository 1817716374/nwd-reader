#include "internal.hpp"
namespace nwd::detail {
template <class T>
static const T &owned(const std::vector<T> &values, Id owner) {
  const auto it =
      std::lower_bound(values.begin(), values.end(), owner,
                       [](const T &value, Id id) { return value.owner < id; });
  require(it != values.end() && it->owner == owner,
          "inline projection missing source payload");
  return *it;
}
static Id mapped(const std::vector<InlineObjectBinding> &values, Id owner) {
  return owner == none ? none : owned(values, owner).value;
}
void project_inline_model(Model &model, uint32_t version,
                          const Options &options) {
  require(version < 28 && model.inline_partition &&
              model.hierarchy_graph == 0 && !model.graphs.empty(),
          "inline model projection requires decoded old partition");
  require(model.instances.empty() && model.geometry_references.empty() &&
              model.transforms.empty() && model.materials.empty() &&
              model.appearances.empty() && model.assets.empty() &&
              !model.inline_projection,
          "inline model already projected");
  const auto &graph = model.graphs[0];
  auto projection = std::make_shared<InlineModelProjection>();
  const GeometryObjectArena empty;
  const auto &arena = graph.geometry_arena ? *graph.geometry_arena : empty;
  const auto store_count = model.geometries.size();
  auto add = [&](std::vector<InlineObjectBinding> &table, Id owner,
                 size_t base = 0) {
    require(base + table.size() < none && table.size() < options.max_objects,
            "inline projection table limit");
    table.push_back({owner, static_cast<Id>(base + table.size())});
  };
  // Allocate identities before projecting references. Child objects may appear
  // after their owners; equal values from distinct source objects stay
  // distinct.
  for (Id id = 0; id < graph.objects.size(); ++id) {
    switch (graph.objects[id].type) {
    case 12:
      add(projection->geometry_references, id);
      [[fallthrough]];
    case 94:
    case 95:
    case 96:
    case 103:
    case 104:
    case 105:
    case 181:
      add(projection->geometries, id, store_count);
      break;
    case 93:
      add(projection->geometry_references, id);
      break;
    case 14:
    case 15:
    case 16:
    case 17:
      add(projection->transforms, id);
      break;
    case 3:
    case 54:
      add(projection->materials, id);
      break;
    case 4:
    case 55:
      add(projection->appearances, id);
      break;
    case 185:
      add(projection->assets, id);
      break;
    }
  }
  uint64_t remaining = options.max_decoded_chunk;
  auto budget = [&](size_t count, size_t width) {
    require(count <= remaining / width, "inline projection byte limit");
    remaining -= uint64_t(count) * width;
  };
  auto primitive = [&](Id owner) -> const GeometryPrimitiveObject & {
    return graph.objects[owner].type == 12
               ? owned(arena.legacy_geometries, owner).primitive
               : owned(arena.primitives, owner);
  };
  budget(projection->geometries.size(), sizeof(Geometry));
  budget(projection->geometry_references.size(), sizeof(GeometryReference));
  budget(projection->transforms.size(), sizeof(Transform));
  budget(projection->materials.size(), sizeof(Material));
  budget(projection->appearances.size(), sizeof(Appearance));
  budget(projection->assets.size(), sizeof(Asset));
  budget(model.inline_partition->shape_bindings.size(), sizeof(Instance));
  for (const auto &binding : projection->geometries) {
    const auto type = graph.objects[binding.owner].type;
    if (type == 103 || type == 104 || type == 105 || type == 181) {
      budget(owned(arena.special, binding.owner).data.text.size(), 1);
      continue;
    }
    const auto &p = primitive(binding.owner);
    if (p.coordinates != none)
      budget(owned(arena.coordinates, p.coordinates).values.size(),
             sizeof(float));
    budget(p.vertex_count, sizeof(uint32_t));
    budget(p.lengths.size(), sizeof(uint32_t));
    uint64_t used = 0;
    for (auto count : p.lengths)
      used += count;
    require(used <= options.max_objects, "inline primitive index limit");
    budget(static_cast<size_t>(used), sizeof(uint32_t));
    for (Id attribute : p.attributes)
      if (attribute != none) {
        const auto &a = owned(arena.attributes, attribute).data;
        budget(1, sizeof(AttributeArray));
        budget(a.packed_palette.size(), 1);
        budget(p.vertex_count, sizeof(int32_t));
      }
  }
  require(store_count + projection->geometries.size() < none &&
              store_count + projection->geometries.size() <=
                  options.max_objects,
          "inline model geometry count limit");
  model.geometries.resize(store_count + projection->geometries.size());
  parallel_for(projection->geometries.size(), options.threads, [&](size_t i) {
    const auto binding = projection->geometries[i];
    const auto type = graph.objects[binding.owner].type;
    if (type == 103 || type == 104 || type == 105 || type == 181) {
      model.geometries[binding.value] =
          owned(arena.special, binding.owner).data;
      return;
    }
    const auto &p = primitive(binding.owner);
    Geometry g;
    g.type = p.type;
    g.flags = p.flags;
    if (p.coordinates != none) {
      const auto &coordinates = owned(arena.coordinates, p.coordinates);
      require(p.vertex_count <= coordinates.indices.size(),
              "inline coordinate prefix");
      g.coordinates = coordinates.values;
      g.coordinate_indices.assign(coordinates.indices.begin(),
                                  coordinates.indices.begin() + p.vertex_count);
      g.coordinate_quantization = coordinates.quantization;
    } else
      require(p.vertex_count == 0, "inline vertices without coordinates");
    g.strip_lengths = p.lengths;
    size_t used = 0;
    for (auto length : p.lengths)
      used += length;
    if (p.implicit_indices) {
      g.strip_indices.resize(used);
      for (size_t j = 0; j < used; ++j)
        g.strip_indices[j] = static_cast<uint32_t>(j);
    } else {
      require(used <= p.indices.size(), "inline strip index prefix");
      g.strip_indices.assign(p.indices.begin(), p.indices.begin() + used);
    }
    for (Id attribute : p.attributes)
      if (attribute != none) {
        const auto &source = owned(arena.attributes, attribute).data;
        require(p.vertex_count <= source.indices.size(),
                "inline attribute prefix");
        AttributeArray a;
        a.raw = source.raw;
        a.finite = source.finite;
        a.quantization_bounds = source.quantization_bounds;
        a.component_bits = source.component_bits;
        a.type = source.type;
        a.flag = source.flag;
        a.bits = source.bits;
        a.palette_count = source.palette_count;
        a.packed_palette = source.packed_palette;
        a.indices.assign(source.indices.begin(),
                         source.indices.begin() + p.vertex_count);
        g.attributes.push_back(std::move(a));
      }
    model.geometries[binding.value] = std::move(g);
  });
  model.geometry_references.resize(projection->geometry_references.size());
  for (const auto &binding : projection->geometry_references) {
    GeometryReference ref;
    if (graph.objects[binding.owner].type == 12)
      ref.geometry = mapped(projection->geometries, binding.owner);
    else {
      const auto &source = owned(arena.references, binding.owner);
      ref = source.fields;
      if (source.uses_store) {
        require(source.data_id && source.data_id <= store_count,
                "inline geometry reference outside source store");
        ref.geometry = source.data_id - 1;
      } else
        ref.geometry = mapped(projection->geometries, source.inline_geometry);
    }
    model.geometry_references[binding.value] = ref;
  }
  model.transforms.resize(projection->transforms.size());
  for (const auto &binding : projection->transforms) {
    const auto &source = graph.objects[binding.owner];
    Transform t;
    t.type = source.type;
    const size_t count = t.type == 14   ? 3
                         : t.type == 15 ? 7
                         : t.type == 16 ? 8
                                        : 16;
    require(source.numbers.size() == count, "inline transform fields");
    std::copy(source.numbers.begin(), source.numbers.end(), t.values.begin());
    model.transforms[binding.value] = t;
  }
  model.source_transform_count = model.transforms.size();
  model.materials.resize(projection->materials.size());
  for (const auto &binding : projection->materials) {
    if (graph.objects[binding.owner].type == 3)
      model.materials[binding.value] =
          owned(arena.legacy_materials, binding.owner).fields;
    else {
      const auto &source = graph.objects[binding.owner].numbers;
      require(source.size() == 14, "inline material fields");
      for (size_t i = 0; i < 14; ++i)
        model.materials[binding.value].values[i] =
            static_cast<float>(source[i]);
    }
  }
  model.assets.resize(projection->assets.size());
  for (const auto &binding : projection->assets) {
    const auto &source = graph.objects[binding.owner];
    auto text = [&](size_t at) {
      require(at < source.strings.size(), "inline asset string field");
      const auto value = resolve_string(graph, source.strings[at]);
      budget(value.size(), 1);
      return std::string(value);
    };
    auto &a = model.assets[binding.value];
    size_t string = 0;
    a.json = text(string++);
    if (version >= 447)
      a.extra = text(string++);
    require(!source.integers.empty() &&
                source.integers[0] + 1 == source.integers.size(),
            "inline asset file fields");
    for (size_t i = 1; i < source.integers.size(); ++i) {
      auto name = text(string++);
      if (!source.integers[i])
        a.files.emplace_back(std::move(name), text(string++));
    }
    require(string == source.strings.size(), "inline asset string extent");
    for (const auto &file : graph.embedded_files)
      if (file.owner == binding.owner) {
        budget(file.bytes.size(), 1);
        budget(file.name.size() + file.prefix.size() + file.suffix.size(), 1);
        a.embedded_files.push_back(file);
        a.embedded_files.back().owner = none;
      }
  }
  model.appearances.resize(projection->appearances.size());
  for (const auto &binding : projection->appearances) {
    auto &a = model.appearances[binding.value];
    const auto &source = graph.objects[binding.owner];
    if (source.type == 4) {
      const auto &legacy = owned(arena.legacy_appearances, binding.owner);
      a.material = mapped(projection->materials, legacy.materials[0]);
      a.flags = a.material == none ? 0 : 1;
      if (legacy.saved_boolean || legacy.saved_enum ||
          !(legacy.excluded_fields & (1ull << 25)))
        projection->legacy_appearance_fields.push_back(binding.owner);
    } else {
      require(source.references.size() == 2 && source.integers.size() == 4,
              "inline appearance fields");
      for (const auto ref : source.references)
        require(ref.graph == 0, "inline appearance graph");
      a.flags = source.flags;
      a.material = mapped(projection->materials, source.references[0].object);
      a.asset = mapped(projection->assets, source.references[1].object);
      std::copy(source.integers.begin(), source.integers.end(),
                a.overrides.begin());
    }
  }
  for (const auto &binding : model.inline_partition->shape_bindings) {
    InlineObjectBinding source{binding.owner, none};
    if (binding.status == InlineShapePathStatus::resolved) {
      const auto &shape = owned(arena.shapes, binding.owner);
      Instance instance;
      instance.path = binding.path;
      instance.geometry_reference =
          mapped(projection->geometry_references, shape.geometry_reference);
      instance.transform = mapped(projection->transforms, shape.transform);
      instance.appearance = mapped(projection->appearances, shape.appearance);
      instance.flags = shape.flags;
      instance.primitive_count = shape.primitive_count;
      instance.bits = shape.bits;
      instance.precision = shape.precision;
      require(shape.auxiliary_transform == none,
              "unexpected old auxiliary transform");
      source.value = static_cast<Id>(model.instances.size());
      model.instances.push_back(instance);
    }
    projection->instances.push_back(source);
  }
  model.inline_projection = std::move(projection);
}
} // namespace nwd::detail
