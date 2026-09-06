#include "internal.hpp"
namespace nwd {
ModelIndex::ModelIndex(const Model &m) {
  using detail::require;
  require(!m.paths.empty(), "path index requires loaded hierarchy");
  require(m.paths.size() < none && m.instances.size() < none,
          "index exceeds 32-bit IDs");
  size_t n = m.paths.size();
  child_offsets_.resize(n + 1);
  instance_offsets_.resize(n + 1);
  require(m.paths[0].parent == none, "root path has parent");
  for (size_t i = 1; i < n; ++i) {
    auto parent = m.paths[i].parent;
    // The reader emits preorder paths. This also rejects cycles and extra
    // roots.
    require(parent < i, "path parent must precede child");
    ++child_offsets_[parent + 1];
  }
  for (const auto &instance : m.instances) {
    require(instance.path < n, "instance path outside hierarchy");
    ++instance_offsets_[instance.path + 1];
  }
  for (size_t i = 1; i <= n; ++i) {
    child_offsets_[i] += child_offsets_[i - 1];
    instance_offsets_[i] += instance_offsets_[i - 1];
  }
  children_.resize(n - 1);
  instances_.resize(m.instances.size());
  auto next = child_offsets_;
  for (Id i = 1; i < n; ++i)
    children_[next[m.paths[i].parent]++] = i;
  next = instance_offsets_;
  for (Id i = 0; i < m.instances.size(); ++i)
    instances_[next[m.instances[i].path]++] = i;
}
std::span<const Id> ModelIndex::children(Id path) const {
  detail::require(!child_offsets_.empty() && path < child_offsets_.size() - 1,
                  "child query path outside hierarchy");
  return std::span(children_).subspan(
      child_offsets_[path], child_offsets_[path + 1] - child_offsets_[path]);
}
std::span<const Id> ModelIndex::instances(Id path) const {
  detail::require(!instance_offsets_.empty() &&
                      path < instance_offsets_.size() - 1,
                  "instance query path outside hierarchy");
  return std::span(instances_)
      .subspan(instance_offsets_[path],
               instance_offsets_[path + 1] - instance_offsets_[path]);
}
size_t ModelIndex::storage_bytes() const {
  return (child_offsets_.size() + children_.size() + instance_offsets_.size() +
          instances_.size()) *
         sizeof(Id);
}
const Object &resolve_object(const Model &m, Reference r) {
  detail::require(r.graph < m.graphs.size(), "object graph outside model");
  const auto &graph = m.graphs[r.graph];
  detail::require(r.object < graph.objects.size(), "object outside graph");
  return graph.objects[r.object];
}
Reference path_reference(const Model &m, Id path) {
  detail::require(path < m.paths.size(), "path outside hierarchy");
  if (path == 0) {
    detail::require(!m.graphs.empty() && !m.graphs[0].roots.empty(),
                    "missing partition root");
    return {0, m.graphs[0].roots[0]};
  }
  return {1, m.paths[path].object};
}
const Object &path_object(const Model &m, Id path) {
  return resolve_object(m, path_reference(m, path));
}
std::string_view resolve_string(const ObjectGraph &graph, Id id) {
  if (id == none)
    return {};
  detail::require(id < graph.strings.size(), "string outside graph");
  return graph.strings[id];
}
} // namespace nwd
