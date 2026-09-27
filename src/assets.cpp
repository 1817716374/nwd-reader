#include "internal.hpp"
#include "json.hpp"
namespace nwd {
namespace {
const Object &protein_object(const ObjectGraph &graph, Id owner,
                             uint32_t type) {
  detail::require(owner < graph.objects.size() &&
                      graph.objects[owner].type == type,
                  "Protein field object type or identity");
  return graph.objects[owner];
}
void protein_string(const ObjectGraph &graph, Id value) {
  detail::require(value == none || value < graph.strings.size(),
                  "Protein field string outside graph");
}
void protein_schema_reference(const ObjectGraph &graph, Reference ref, Id gid) {
  detail::require(ref.object == none ||
                      (ref.graph == gid && ref.object < graph.objects.size() &&
                       graph.objects[ref.object].type == 183),
                  "Protein schema reference type or graph");
}
} // namespace
ProteinAssetFields protein_asset_fields(const ObjectGraph &graph, Id owner,
                                        uint32_t version, Id gid) {
  using detail::require;
  const auto &o = protein_object(graph, owner, 182);
  require(o.strings.size() >= 2 &&
              o.references.size() == (version >= 254 ? 1u : 0u) &&
              o.bytes.size() == (version >= 411 ? 16u : 0u) &&
              o.integers.size() == (version >= 415 ? 3u : 0u),
          "Protein asset field layout does not match wire version");
  for (Id string : o.strings)
    protein_string(graph, string);
  ProteinAssetFields out;
  out.library_id = o.strings[0];
  out.definition_id = o.strings[1];
  if (version >= 254) {
    protein_schema_reference(graph, o.references[0], gid);
    out.schema = o.references[0];
  }
  if (version >= 411) {
    out.guid.emplace();
    std::copy(o.bytes.begin(), o.bytes.end(), out.guid->begin());
  }
  size_t at = 2;
  const auto strings = std::span<const Id>(o.strings);
  if (version >= 415) {
    auto list = [&](Id n) {
      require(n <= strings.size() - at, "Protein asset string-list extent");
      auto values = strings.subspan(at, n);
      at += n;
      return values;
    };
    out.thumbnails = list(o.integers[0]);
    out.categories = list(o.integers[1]);
    out.keywords = list(o.integers[2]);
  }
  auto text = [&]() {
    require(at < strings.size(), "Protein asset string field absent");
    return strings[at++];
  };
  if (version >= 417)
    out.ui_name = text();
  if (version >= 429)
    out.default_preview_id = text();
  if (version >= 430)
    out.description = text();
  require(at == strings.size(), "Protein asset unconsumed string fields");
  return out;
}
ProteinSchemaFields protein_schema_fields(const ObjectGraph &graph, Id owner,
                                          Id gid) {
  const auto &o = protein_object(graph, owner, 183);
  detail::require(o.strings.size() == 2 && o.references.size() == 1,
                  "Protein schema field layout");
  for (Id string : o.strings)
    protein_string(graph, string);
  protein_schema_reference(graph, o.references[0], gid);
  return {o.strings[0], o.strings[1], o.bytes, o.references[0]};
}
struct ProteinGraphIndex::Impl {
  struct Range {
    size_t begin = 0, count = 0;
  };
  struct Node {
    bool asset = false;
    Range connections, uris;
    std::vector<Range> properties;
  };
  const ObjectGraph *graph;
  std::vector<Node> nodes;
  std::vector<ProteinConnection> edges;
  std::vector<ProteinUriSlot> slots;
  const Node &node(Id id) const {
    detail::require(id < nodes.size() && nodes[id].asset,
                    "Protein asset outside indexed graph");
    return nodes[id];
  }
  Impl(const ObjectGraph &g, Id gid) : graph(&g), nodes(g.objects.size()) {
    using detail::require;
    require(g.objects.size() < none, "Protein graph ID limit");
    for (Id owner = 0; owner < g.objects.size(); ++owner) {
      const auto &o = g.objects[owner];
      if (o.type != 182)
        continue;
      auto &n = nodes[owner];
      n.asset = true;
      n.connections.begin = edges.size();
      n.uris.begin = slots.size();
      require(o.protein_properties.size() < none, "Protein property ID limit");
      n.properties.resize(o.protein_properties.size());
      for (Id property = 0; property < o.protein_properties.size();
           ++property) {
        const auto &p = o.protein_properties[property];
        require(p.connections.size() < none, "Protein connection ID limit");
        Id native = 0;
        for (Id ordinal = 0; ordinal < p.connections.size(); ++ordinal) {
          const auto ref = p.connections[ordinal];
          require(ref.object == none ||
                      (ref.graph == gid && ref.object < g.objects.size() &&
                       g.objects[ref.object].type == 182),
                  "Protein connection must reference same-graph asset");
          edges.push_back({owner, property, ordinal, ref.object,
                           ref.object == none ? none : native++, p.enabled});
        }
        if (p.type != 13)
          continue;
        require(p.strings.size() == p.count, "Protein URI count mismatch");
        auto &range = n.properties[property];
        range = {slots.size(), p.count};
        for (Id ordinal = 0; ordinal < p.count; ++ordinal) {
          const auto string = p.strings[ordinal];
          require(string == none || string < g.strings.size(),
                  "Protein URI string outside graph");
          slots.push_back({owner, property, ordinal, string, none});
        }
        for (Id id : p.embedded_files) {
          require(id < g.embedded_files.size(),
                  "Protein URI file outside graph");
          const auto &file = g.embedded_files[id];
          require(file.owner == owner && file.ordinal < p.count,
                  "Protein URI embedded ownership mismatch");
          auto &slot = slots[range.begin + file.ordinal];
          require(slot.embedded_file == none,
                  "duplicate Protein URI embedded ordinal");
          slot.embedded_file = id;
        }
      }
      n.connections.count = edges.size() - n.connections.begin;
      n.uris.count = slots.size() - n.uris.begin;
    }
  }
};
ProteinGraphIndex::ProteinGraphIndex(const ObjectGraph &g, Id gid)
    : impl(std::make_shared<Impl>(g, gid)) {}
const ObjectGraph &ProteinGraphIndex::graph() const { return *impl->graph; }
std::span<const ProteinConnection>
ProteinGraphIndex::connections(Id owner) const {
  const auto r = impl->node(owner).connections;
  return std::span<const ProteinConnection>(impl->edges)
      .subspan(r.begin, r.count);
}
std::span<const ProteinUriSlot> ProteinGraphIndex::uris(Id owner) const {
  const auto r = impl->node(owner).uris;
  return std::span<const ProteinUriSlot>(impl->slots).subspan(r.begin, r.count);
}
const ProteinUriSlot &ProteinGraphIndex::uri(Id owner, Id property,
                                             Id ordinal) const {
  const auto &n = impl->node(owner);
  detail::require(property < n.properties.size(),
                  "Protein URI property outside object");
  const auto r = n.properties[property];
  detail::require(ordinal < r.count, "Protein URI ordinal outside property");
  return impl->slots[r.begin + ordinal];
}
std::vector<ProteinReachableAsset> ProteinGraphIndex::reachable(Id root) const {
  impl->node(root);
  // Query-local sparse state: many unrelated roots must not clear the entire
  // graph for every request. Shared immutable indices remain thread-safe.
  std::unordered_map<Id, size_t> positions{{root, 0}};
  std::vector<ProteinReachableAsset> out{{root, true}};
  for (size_t i = 0; i < out.size(); ++i)
    for (const auto &edge : connections(out[i].object))
      if (edge.target != none &&
          positions.try_emplace(edge.target, out.size()).second) {
        out.push_back({edge.target, false});
      }
  std::vector<Id> enabled{root};
  for (size_t i = 0; i < enabled.size(); ++i)
    for (const auto &edge : connections(enabled[i]))
      if (edge.enabled && edge.target != none &&
          !out[positions.at(edge.target)].enabled_path) {
        out[positions.at(edge.target)].enabled_path = true;
        enabled.push_back(edge.target);
      }
  return out;
}
AssetDescription describe_asset(const Asset &asset) {
  detail::require(
      !asset.protein,
      "legacy Protein asset retains an object graph, not asset JSON");
  AssetDescription out;
  if (asset.json.empty())
    return out;
  try {
    auto j = nlohmann::json::parse(asset.json);
    detail::require(j.at("version") == 2, "unsupported asset JSON version");
    if (j.contains("userassets"))
      out.roots = j.at("userassets").get<std::vector<std::string>>();
    for (const auto &[id, v] : j.at("materials").items()) {
      AssetNode node;
      node.id = id;
      node.definition = v.value("definition", std::string{});
      node.name = v.value("tag", std::string{});
      if (v.contains("properties"))
        for (const auto &[cat, props] : v.at("properties").items()) {
          if (!props.is_object())
            continue;
          for (const auto &[name, p] : props.items()) {
            if (!p.is_object())
              continue;
            AssetParameter a;
            a.category = cat;
            a.name = name;
            if (p.contains("values"))
              a.values_json = p.at("values").dump();
            if (p.contains("connections"))
              a.connections =
                  p.at("connections").get<std::vector<std::string>>();
            a.connections_enabled = p.value("IsConnectionEnabled", true);
            node.parameters.push_back(std::move(a));
            if (cat == "uris" && p.contains("values"))
              for (const auto &u : p.at("values"))
                if (u.is_string())
                  out.uris.push_back(
                      {id, name, u.get<std::string>(), name == "thumbnail"});
          }
        }
      out.nodes.push_back(std::move(node));
    }
    return out;
  } catch (const nlohmann::json::exception &e) {
    throw Error(std::string("invalid asset JSON: ") + e.what());
  }
}
} // namespace nwd
