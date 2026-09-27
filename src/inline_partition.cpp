#include "internal.hpp"
namespace nwd::detail {
void bind_inline_partition(Model &model, const Options &options) {
  require(!model.graphs.empty(), "inline partition graph missing");
  const auto &graph = model.graphs[0];
  require(graph.roots.size() == 1 && graph.roots[0] < graph.objects.size() &&
              graph.objects[graph.roots[0]].type == 32,
          "inline partition root type");
  const auto root = graph.roots[0];
  auto index = std::make_shared<InlinePartitionIndex>();
  std::vector<Path> paths;
  std::vector<Id> canonical;
  struct Group {
    uint32_t offset = 0, count = 0;
  };
  std::vector<Group> groups;
  std::unordered_map<uint64_t, Id> sequences;
  auto key = [](Id parent, Id object) {
    return (uint64_t(parent) << 32) | object;
  };
  auto logical = [](uint32_t type) {
    return type == 22 || type == 25 || type == 32 || type == 53;
  };
  struct Frame {
    Id object, occurrence;
    size_t child = 0;
  };
  std::vector<Frame> stack;
  std::vector<bool> active(graph.objects.size());
  auto append = [&](Id object, Id parent, bool spatial) {
    require(object < graph.objects.size() && !active[object],
            "invalid/cyclic inline hierarchy");
    require(stack.size() < 512, "inline hierarchy depth limit");
    const auto type = graph.objects[object].type;
    require(spatial ? type == 1 || type == 2 || type == 13 : logical(type),
            "inline hierarchy child type");
    auto &out = spatial ? index->spatial_occurrences : paths;
    require(out.size() < options.max_objects && out.size() < none,
            "inline occurrence count limit");
    const auto occurrence = static_cast<Id>(out.size());
    out.push_back({parent, object});
    if (!spatial) {
      auto [it, inserted] = sequences.emplace(
          key(parent == none ? none : canonical[parent], object),
          static_cast<Id>(groups.size()));
      if (inserted)
        groups.emplace_back();
      canonical.push_back(it->second);
      ++groups[it->second].count;
    }
    active[object] = true;
    stack.push_back({object, occurrence});
    return occurrence;
  };
  auto expand = [&](Id object, bool spatial) {
    auto first = append(object, none, spatial);
    while (!stack.empty()) {
      auto &frame = stack.back();
      const auto &node = graph.objects[frame.object];
      if (frame.child == node.children.size()) {
        active[frame.object] = false;
        stack.pop_back();
      } else {
        const auto child = node.children[frame.child++];
        if (spatial && node.type == 2)
          require(child < graph.objects.size() &&
                      graph.objects[child].type == 13,
                  "inline spatial leaf child type");
        require(node.type != 13 && (spatial || node.type != 22),
                "inline terminal has children");
        append(child, frame.occurrence, spatial);
      }
    }
    return first;
  };
  expand(root, false);
  uint32_t offset = 0;
  for (auto &group : groups) {
    group.offset = offset;
    offset += group.count;
  }
  index->path_candidates.resize(paths.size());
  auto next = groups;
  for (Id i = 0; i < paths.size(); ++i)
    index->path_candidates[next[canonical[i]].offset++] = i;
  std::unordered_map<Id, Id> bindings;
  auto bind_path = [&](Id owner) {
    if (owner == none)
      return none;
    auto found = bindings.find(owner);
    if (found != bindings.end())
      return found->second;
    require(owner < graph.objects.size() && (graph.objects[owner].type == 44 ||
                                             graph.objects[owner].type == 73),
            "inline shape path type");
    const auto &refs = graph.objects[owner].references;
    InlinePathBinding value;
    value.owner = owner;
    Id group = none;
    bool matches = !refs.empty();
    for (const auto ref : refs) {
      require(ref.graph == 0 && ref.object < graph.objects.size() &&
                  logical(graph.objects[ref.object].type),
              "inline path node reference");
      if (!matches)
        continue;
      auto entry = sequences.find(key(group, ref.object));
      if (entry == sequences.end())
        matches = false;
      else
        group = entry->second;
    }
    if (matches) {
      value.offset = groups[group].offset;
      value.count = groups[group].count;
      value.status = value.count == 1 ? InlinePathStatus::unique
                                      : InlinePathStatus::ambiguous;
    } else if (!refs.empty())
      value.status = InlinePathStatus::no_exact_match;
    const auto id = static_cast<Id>(index->path_bindings.size());
    index->path_bindings.push_back(value);
    bindings.emplace(owner, id);
    return id;
  };
  std::optional<uint64_t> links_ready;
  for (const auto &partition : graph.legacy_partitions)
    if (partition.owner == root)
      links_ready = partition.path_links_ready_offset;
  if (graph.geometry_arena)
    for (const auto &shape : graph.geometry_arena->shapes) {
      require(shape.owner < graph.objects.size() &&
                  graph.objects[shape.owner].type == 13 &&
                  shape.path_encoding == GeometryShapePath::object,
              "inline shape identity/encoding");
      InlineShapeBinding binding;
      binding.owner = shape.owner;
      binding.path_binding = bind_path(shape.path_object);
      if (binding.path_binding != none) {
        const auto &refs = graph.objects[shape.path_object].references;
        const auto &candidates = index->path_bindings[binding.path_binding];
        if (refs.empty())
          binding.status = InlineShapePathStatus::empty_path;
        else if (refs.front().object != root)
          binding.status = InlineShapePathStatus::different_root;
        else if (graph.objects[refs.back().object].type != 22)
          binding.status = InlineShapePathStatus::non_geometry_terminal;
        else if (!links_ready)
          binding.status = InlineShapePathStatus::context_required;
        else if (graph.objects[shape.owner].stream_offset < *links_ready)
          binding.status = InlineShapePathStatus::links_not_ready;
        else if (!candidates.count)
          binding.status = InlineShapePathStatus::no_link;
        else {
          binding.path = index->path_candidates[candidates.offset];
          binding.status = InlineShapePathStatus::resolved;
        }
      }
      index->shape_bindings.push_back(binding);
    }
  for (const auto &partition : graph.legacy_partitions) {
    require(partition.owner < graph.objects.size() &&
                graph.objects[partition.owner].type == 32,
            "inline spatial partition owner");
    const auto ref = partition.spatial_root;
    Id occurrence = none;
    if (ref.object != none) {
      require(ref.graph == 0 && ref.object < graph.objects.size() &&
                  (graph.objects[ref.object].type == 1 ||
                   graph.objects[ref.object].type == 2),
              "inline spatial root reference");
      occurrence = expand(ref.object, true);
    }
    index->spatial_roots.push_back({partition.owner, occurrence});
  }
  // Commit only after both domains validate; raw graphs and shared payloads
  // are retained. Spatial occurrences are not guessed to be fragment slots.
  model.paths = std::move(paths);
  model.hierarchy_graph = 0;
  model.inline_partition = std::move(index);
}
} // namespace nwd::detail
