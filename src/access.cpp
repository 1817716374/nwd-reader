#include "internal.hpp"
#include "xref_string.hpp"
namespace nwd {
std::optional<EmbeddedReference>
parse_embedded_reference(std::u16string_view path) {
  path = path.substr(0, path.find(u'\0'));
  if (!path.starts_with(u"nwd:///"))
    return {};
  // CrackEmbedded starts at length - 2, so a final '#' is part of the
  // resource name only when there is another separator before it.
  const auto separator = path.rfind(u'#', path.size() - 2);
  if (separator == path.npos)
    return {};
  return EmbeddedReference{std::u16string(path.substr(7, separator - 7)),
                           "nwd:" +
                               detail::xref_utf8(path.substr(separator + 1)),
                           path.starts_with(u"nwd:///.#")};
}
struct EmbeddedResourceIndex::Impl {
  std::unordered_map<std::string_view, std::vector<Id>> entries;
  explicit Impl(std::span<const Chunk> chunks) {
    detail::require(chunks.size() < none, "embedded resource directory limit");
    for (Id i = 0; i < chunks.size(); ++i) {
      auto name = std::string_view(chunks[i].name);
      name = name.substr(0, name.find('\0'));
      if (name.starts_with("nwd:"))
        entries[name].push_back(i);
    }
  }
};
EmbeddedResourceIndex::EmbeddedResourceIndex(std::span<const Chunk> chunks)
    : impl(std::make_shared<Impl>(chunks)) {}
EmbeddedResourceLookup
EmbeddedResourceIndex::find(std::string_view chunk_name) const {
  chunk_name = chunk_name.substr(0, chunk_name.find('\0'));
  const auto found = impl->entries.find(chunk_name);
  if (found == impl->entries.end())
    return {};
  return {found->second.front(), found->second};
}
namespace {
ExternalReferenceScope reference_scope(std::span<const Chunk> chunks,
                                       const ProductData *products) {
  ExternalReferenceScope out;
  detail::require(chunks.size() < none, "XRef directory entry limit");
  for (Id i = 0; i < chunks.size(); ++i) {
    if (chunks[i].name != "LcOpXRefTable")
      continue;
    if (out.block != none)
      return {ExternalReferenceScopeStatus::ambiguous, none, nullptr};
    out.block = i;
    out.status = ExternalReferenceScopeStatus::unavailable;
  }
  if (out.block == none || !products || out.block >= products->blocks.size() ||
      out.block >= products->chunks.size() ||
      products->chunks[out.block].name != chunks[out.block].name)
    return out;
  const auto &block = products->blocks[out.block];
  if (block.status != ProductStatus::decoded)
    return out;
  out.table = std::get_if<ExternalReferenceTable>(&block.value);
  if (out.table)
    out.status = ExternalReferenceScopeStatus::available;
  return out;
}
} // namespace
ExternalReferenceScope external_reference_scope(const Scene &scene) {
  return reference_scope(scene.chunks, scene.products.get());
}
ExternalReferenceScope external_reference_scope(const ProductData &products) {
  return reference_scope(products.chunks, &products);
}
ExternalReferenceScope external_reference_scope(std::span<const Chunk> chunks) {
  return reference_scope(chunks, nullptr);
}
std::u16string expand_embedded_reference_path(std::u16string_view path,
                                              std::u16string_view filename) {
  path = path.substr(0, path.find(u'\0'));
  filename = filename.substr(0, filename.find(u'\0'));
  if (filename.empty() || !path.starts_with(u"nwd:///.#"))
    return std::u16string(path);
  std::u16string result(u"nwd:///");
  result.append(filename);
  result.push_back(u'#');
  result.append(path.substr(9));
  return result;
}
struct ExternalReferenceIndex::Impl {
  const ExternalReferenceTable *table;
  std::unordered_map<std::string_view, std::vector<Id>> entries;
  struct ReadEntries {
    std::vector<Id> entries;
    detail::XRefString target;
    bool key_replaced = false;
  };
  std::unordered_map<std::u16string, ReadEntries> read_entries;
  ExternalReadPathResolution query(ExternalReadPathResolution result) const {
    if (result.path.empty())
      return result;
    const auto found = read_entries.find(result.path);
    if (found == read_entries.end())
      return result;
    const auto &read = found->second;
    result.status = ExternalReadPathStatus::remapped;
    result.entry = read.entries.back();
    result.entries = read.entries;
    result.path = read.target.value;
    result.table_encoding_replaced = read.key_replaced || !read.target.valid;
    return result;
  }
  explicit Impl(const ExternalReferenceTable &source) : table(&source) {
    detail::require(source.path_remaps.size() < none, "XRef index entry limit");
    for (Id i = 0; i < source.path_remaps.size(); ++i) {
      entries[source.path_remaps[i].first].push_back(i);
      auto key = detail::xref_string(source.path_remaps[i].first);
      auto &read = read_entries[std::move(key.value)];
      read.entries.push_back(i);
      read.key_replaced = !key.valid;
      read.target = detail::xref_string(source.path_remaps[i].second);
    }
  }
};
ExternalReferenceIndex::ExternalReferenceIndex(
    const ExternalReferenceTable &table)
    : impl(std::make_shared<Impl>(table)) {}
ExternalPathResolution
ExternalReferenceIndex::resolve(const ExternalGeometry &g) const {
  if (g.null_strings[2])
    return {ExternalPathStatus::null_path, {}, {}};
  ExternalPathResolution result{
      ExternalPathStatus::original, g.source_path, {}};
  // Native ReadEntry bypasses lookup for both null and empty input.
  if (g.source_path.empty())
    return result;
  const auto found = impl->entries.find(g.source_path);
  if (found == impl->entries.end())
    return result;
  result.entries = found->second;
  if (found->second.size() != 1) {
    result.status = ExternalPathStatus::ambiguous;
    return result;
  }
  result.status = ExternalPathStatus::remapped;
  result.path = impl->table->path_remaps[found->second.front()].second;
  return result;
}
ExternalReadPathResolution
ExternalReferenceIndex::resolve_read_path(const ExternalGeometry &g) const {
  ExternalReadPathResolution result;
  if (g.null_strings[2]) {
    result.status = ExternalReadPathStatus::null_path;
    return result;
  }
  auto source = detail::xref_string(g.source_path, true);
  result.path = std::move(source.value);
  // ReadWideString sets stream error before ReadEntry can consult the table.
  if (!source.valid) {
    result.status = ExternalReadPathStatus::invalid_encoding;
    return result;
  }
  return impl->query(std::move(result));
}
ExternalReadPathResolution
ExternalReferenceIndex::query_path(std::u16string_view path) const {
  ExternalReadPathResolution result;
  result.path = path.substr(0, path.find(u'\0'));
  return impl->query(std::move(result));
}
TextStyleResolution resolve_text_style(const Model &model, const Geometry &g) {
  if (g.type != 103)
    return {};
  TextStyleResolution out{TextStyleStatus::unavailable};
  const auto *f = g.text_fields.get();
  if (!f || f->uses_shared_nodes) {
    const int64_t index =
        f ? f->shared_node_index
          : (g.text_style == none ? -1 : int64_t(g.text_style));
    if (index < 0)
      return {TextStyleStatus::null_style};
    out.graph = &model.shared_nodes;
    if (uint64_t(index) >= out.graph->roots.size())
      return out;
    out.object = out.graph->roots[size_t(index)];
  } else {
    out.object = f->inline_style.object;
    if (out.object == none)
      return {TextStyleStatus::null_style};
    if (f->record_graph)
      out.graph = f->record_graph.get();
    else if (f->inline_style.graph == none)
      out.graph = &model.shared_nodes;
    else if (f->inline_style.graph < model.graphs.size())
      out.graph = &model.graphs[f->inline_style.graph];
  }
  if (out.object == none)
    out.status = TextStyleStatus::null_style;
  else if (out.graph && out.object < out.graph->objects.size())
    out.status = out.graph->objects[out.object].type == 102
                     ? TextStyleStatus::resolved
                     : TextStyleStatus::wrong_type;
  return out;
}
} // namespace nwd
namespace nwd {
const AnimationObject *animation_object(const ObjectGraph &graph, Id id) {
  detail::require(id < graph.objects.size(), "animation object outside graph");
  if (graph.objects[id].type < 150 || graph.objects[id].type > 167)
    return nullptr;
  const auto &records = graph.animation_objects;
  auto it = std::lower_bound(
      records.begin(), records.end(), id,
      [](const AnimationObject &v, Id owner) { return v.owner < owner; });
  return it != records.end() && it->owner == id ? &*it : nullptr;
}
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
  return {m.hierarchy_graph, m.paths[path].object};
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
