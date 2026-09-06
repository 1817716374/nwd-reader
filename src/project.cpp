#include "internal.hpp"
#include <fstream>
#include <map>
#include <set>
#include <unordered_set>
namespace nwd {
namespace {
using namespace detail;
using Matrix = std::array<double, 16>;
Matrix identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
Matrix multiply(const Matrix &a, const Matrix &b) {
  Matrix c{};
  for (unsigned col = 0; col < 4; ++col)
    for (unsigned row = 0; row < 4; ++row)
      for (unsigned k = 0; k < 4; ++k)
        c[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
  return c;
}
Matrix affine(std::span<const double> a) {
  require(a.size() == 12, "invalid base affine size");
  auto out = identity();
  // Native affine storage uses row vectors; public matrices use column vectors.
  for (unsigned col = 0; col < 3; ++col)
    for (unsigned row = 0; row < 3; ++row)
      out[col * 4 + row] = a[col * 3 + row];
  for (unsigned i = 0; i < 3; ++i)
    out[12 + i] = a[9 + i];
  for (double v : out)
    require(std::isfinite(v), "nonfinite base affine");
  return out;
}
Matrix inverse(const Matrix &m) {
  double a[4][8]{};
  for (unsigned row = 0; row < 4; ++row) {
    for (unsigned col = 0; col < 4; ++col)
      a[row][col] = m[col * 4 + row];
    a[row][4 + row] = 1;
  }
  for (unsigned col = 0; col < 4; ++col) {
    unsigned pivot = col;
    for (unsigned row = col + 1; row < 4; ++row)
      if (std::abs(a[row][col]) > std::abs(a[pivot][col]))
        pivot = row;
    require(a[pivot][col] != 0 && std::isfinite(a[pivot][col]),
            "singular source base transform");
    for (unsigned j = 0; j < 8; ++j)
      std::swap(a[pivot][j], a[col][j]);
    double scale = a[col][col];
    for (double &v : a[col])
      v /= scale;
    for (unsigned row = 0; row < 4; ++row)
      if (row != col) {
        double factor = a[row][col];
        for (unsigned j = 0; j < 8; ++j)
          a[row][j] -= factor * a[col][j];
      }
  }
  Matrix out{};
  for (unsigned col = 0; col < 4; ++col)
    for (unsigned row = 0; row < 4; ++row) {
      out[col * 4 + row] = a[row][4 + col];
      require(std::isfinite(out[col * 4 + row]),
              "nonfinite inverse base transform");
    }
  return out;
}
const Object *base_attribute(const Model &m) {
  for (auto ref : path_object(m, 0).attributes) {
    const auto &o = resolve_object(m, ref);
    if (o.type >= 38 && o.type <= 41)
      return &o;
  }
  return nullptr;
}
std::string utf8(const std::filesystem::path &p) {
  auto u = p.u8string();
  return {reinterpret_cast<const char *>(u.data()), u.size()};
}
std::filesystem::path path(std::string s) {
  std::replace(s.begin(), s.end(), '\\', '/');
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}
std::string key(const std::filesystem::path &p) {
  auto s = utf8(p.lexically_normal());
#ifdef _WIN32
  for (char &c : s)
    if (c >= 'A' && c <= 'Z')
      c += 32;
#endif
  return s;
}
double unit(uint32_t u) {
  switch (u) {
  case 0:
    return 1;
  case 1:
    return .01;
  case 2:
    return .001;
  case 3:
    return .3048;
  case 4:
    return .0254;
  case 5:
    return .9144;
  case 6:
    return 1000;
  case 7:
    return 1609.344;
  case 8:
    return .000001;
  case 9:
    return .0000254;
  case 10:
    return .0000000254;
  default:
    throw Error("unsupported project linear unit");
  }
}
struct Resolution {
  std::filesystem::path file;
  std::string status = "missing";
};
template <class F>
void product_selectors(const ProductData &products, F callback) {
  for (Id block = 0; block < products.blocks.size(); ++block) {
    const auto &b = products.blocks[block];
    if (b.status != ProductStatus::decoded)
      continue;
    std::visit(
        [&](const auto &v) {
          using T = std::decay_t<decltype(v)>;
          auto lists = [&](const auto &records, ProductBindingKind kind) {
            for (Id record = 0; record < records.size(); ++record)
              callback(block, record, kind, records[record].node_scope,
                       std::span<const Id>(records[record].paths));
          };
          if constexpr (std::is_same_v<T, PresenterData>) {
            lists(v.material_bindings, ProductBindingKind::presenter_material);
            lists(v.texture_bindings, ProductBindingKind::presenter_texture);
          } else if constexpr (std::is_same_v<T, TextureSpaceOverrides>)
            lists(v.records, ProductBindingKind::texture_space);
          else if constexpr (std::is_same_v<T, HyperlinkOverrides> ||
                             std::is_same_v<T, NodeOverrides>) {
            constexpr auto kind = std::is_same_v<T, HyperlinkOverrides>
                                      ? ProductBindingKind::hyperlink
                                      : ProductBindingKind::node_override;
            for (Id record = 0; record < v.paths.size(); ++record)
              callback(block, record, kind, false,
                       std::span<const Id>(&v.paths[record].path_link, 1));
            for (Id record = 0; record < v.nodes.size(); ++record)
              callback(block, record, kind, true,
                       std::span<const Id>(v.nodes[record].paths));
          }
        },
        b.value);
  }
}
class Loader {
  ProjectOptions options;
  Project out;
  std::map<std::string, Id> cache;
  std::set<std::string> active;
  std::map<std::pair<Id, Id>, ProjectAppearance> overrides;
  std::map<std::tuple<Id, Id, Id>, Matrix> transform_overrides;
  std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>>
      texture_cache;
  void missing(std::string s) {
    out.complete = false;
    if (std::find(out.warnings.begin(), out.warnings.end(), s) ==
        out.warnings.end())
      out.warnings.push_back(std::move(s));
  }
  Resolution choose(const std::vector<std::filesystem::path> &candidates) {
    std::map<std::string, std::filesystem::path> found;
    for (const auto &p : candidates) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(p, ec)) {
        auto c = std::filesystem::canonical(p, ec);
        if (!ec)
          found.emplace(key(c), c);
      }
    }
    if (found.size() > 1)
      return {{}, "ambiguous"};
    if (found.empty())
      return {};
    return {found.begin()->second, "resolved"};
  }
  Resolution
  resolve(const std::filesystem::path &owner, const std::string &requested,
          const std::vector<std::pair<std::string, std::string>> &remaps = {}) {
    auto p = path(requested);
    std::vector<std::filesystem::path> candidates;
    for (const auto &[from, to] : options.remaps)
      if (key(path(from)) == key(p))
        candidates.push_back(to);
    if (!candidates.empty())
      return choose(candidates);
    for (const auto &[from, to] : remaps)
      if (key(path(from)) == key(p)) {
        auto t = path(to);
        candidates.push_back(t.is_absolute() ? t : owner.parent_path() / t);
      }
    if (p.is_relative())
      candidates.push_back(owner.parent_path() / p);
    auto r = choose(candidates);
    if (r.status != "missing")
      return r;
    if (p.is_absolute()) {
      r = choose({p});
      if (r.status != "missing")
        return r;
    }
    candidates = {owner.parent_path() / p.filename()};
    for (const auto &dir : options.search_paths) {
      candidates.push_back(dir / p.filename());
      if (p.is_relative())
        candidates.push_back(dir / p);
    }
    return choose(candidates);
  }
  Id source(const std::filesystem::path &file) {
    auto k = key(file);
    auto found = cache.find(k);
    if (found != cache.end())
      return found->second;
    Document d(file, options.reader);
    ProjectSource s;
    s.path = file;
    s.version = d.version();
    bool nwf =
        std::any_of(d.chunks().begin(), d.chunks().end(), [](const Chunk &c) {
          return c.name.ends_with("LcOpNwfSceneSet");
        });
    if (nwf)
      s.nwf = std::make_shared<NwfData>(d.read_nwf());
    else
      s.scene = std::make_shared<Scene>(d.read_scene());
    if (options.reader.products)
      s.products = s.scene ? s.scene->products
                           : std::make_shared<ProductData>(d.read_products());
    if (s.products) {
      size_t remaining = 0;
      const auto &parsed =
          s.scene ? s.scene->parsed_chunks : s.nwf->parsed_chunks;
      for (size_t i = 0; i < d.chunks().size(); ++i)
        if (s.products->blocks[i].status == ProductStatus::failed ||
            s.products->blocks[i].status == ProductStatus::partial ||
            (!parsed[i] &&
             s.products->blocks[i].status != ProductStatus::decoded))
          ++remaining;
      if (remaining)
        missing(utf8(file) + ": " + std::to_string(remaining) +
                " document blocks remain unparsed; inspect products.blocks");
    }
    auto id = static_cast<Id>(out.sources.size());
    out.sources.push_back(std::move(s));
    cache.emplace(k, id);
    return id;
  }
  const Model &model(Id node) const {
    const auto &n = out.nodes.at(node);
    return out.sources.at(n.source).scene->models.at(n.model);
  }
  bool match(const NwfPath &p, const Model &m, Id id) {
    auto ref = path_reference(m, id);
    const auto &o = resolve_object(m, ref);
    const auto &g = m.graphs[ref.graph];
    if ((p.flags & 1) && p.type != o.type)
      return false;
    if ((p.flags & 4) && p.name != resolve_string(g, o.name))
      return false;
    if (p.flags & 2) {
      if (o.class_name == none || o.class_name >= g.objects.size())
        return false;
      const auto &c = g.objects[o.class_name];
      if (c.strings.empty() || p.class_name != resolve_string(g, c.strings[0]))
        return false;
    }
    if (p.flags & 8) {
      bool same = false;
      for (auto r : o.attributes) {
        const auto &a = resolve_object(m, r);
        if ((a.type == 64 || a.type == 66 || a.type == 67) && !a.wide.empty() &&
            a.wide[0] == p.entity)
          same = true;
      }
      if (!same)
        return false;
    }
    return true;
  }
  template <class Resolve> void bind_saved_paths(Id owner, Resolve resolve) {
    const auto &source = out.sources[out.nodes[owner].source];
    if (!source.products)
      return;
    for (Id block = 0; block < source.products->blocks.size(); ++block) {
      const auto &b = source.products->blocks[block];
      const auto *saved = std::get_if<SavedItems>(&b.value);
      if (b.status != ProductStatus::decoded || !saved)
        continue;
      std::unordered_set<Id> unique;
      require(saved_path_links(*saved,
                               [&](Id id) {
                                 if (!unique.contains(id)) {
                                   require(out.saved_path_bindings.size() +
                                                   unique.size() <
                                               options.reader.max_objects,
                                           "saved path binding resource limit");
                                   unique.insert(id);
                                 }
                                 return true;
                               }),
              "malformed saved path fields");
      std::vector<Id> ids(unique.begin(), unique.end());
      std::sort(ids.begin(), ids.end());
      const auto &name = source.products->chunks[block].name;
      auto split = name.rfind('\\');
      auto prefix = split == name.npos ? std::string{} : name.substr(0, split);
      bool context = source.nwf && saved->implicit_node_map &&
                     prefix == source.nwf->path_map_namespace;
      for (Id id : ids) {
        require(out.saved_path_bindings.size() < options.reader.max_objects,
                "saved path binding resource limit");
        auto &binding = out.saved_path_bindings.emplace_back();
        binding.owner = owner;
        binding.block = block;
        binding.path_link = id;
        if (id == 0 || id == none) {
          binding.status = ProductBindingStatus::empty;
          continue;
        }
        if (context && id == 1 && source.nwf->path_map_kind != 0) {
          binding.status = ProductBindingStatus::resolved;
          binding.node = owner;
          binding.project_root = true;
          continue;
        }
        if (context)
          if (auto target = resolve(id)) {
            binding.status = ProductBindingStatus::resolved;
            binding.node = target->first;
            binding.path = target->second;
            continue;
          }
        missing("unresolved NWF saved path " + name + " ID " +
                std::to_string(id));
      }
    }
  }
  void bind(Id owner, size_t end) {
    const auto source_id = out.nodes[owner].source;
    auto data = out.sources[source_id].nwf;
    const auto products = out.sources[source_id].products;
    auto empty_selector = [](std::span<const Id> paths) {
      return std::all_of(paths.begin(), paths.end(),
                         [](Id id) { return id == 0 || id == none; });
    };
    bool have_product_selectors = false;
    if (products)
      product_selectors(*products, [&](Id, Id, auto, bool, auto paths) {
        have_product_selectors |= !empty_selector(paths);
      });
    bool have_saved_paths = false;
    if (products)
      for (const auto &b : products->blocks)
        if (b.status == ProductStatus::decoded)
          if (const auto *saved = std::get_if<SavedItems>(&b.value))
            require(saved_path_links(*saved,
                                     [&](Id id) {
                                       have_saved_paths |=
                                           id != 0 && id != none;
                                       return true;
                                     }),
                    "malformed saved path fields");
    auto no_saved_target = [](Id) -> std::optional<std::pair<Id, Id>> {
      return {};
    };
    auto unresolved_products = [&] {
      if (products)
        product_selectors(*products,
                          [&](Id block, Id record, ProductBindingKind kind,
                              bool node_scope, std::span<const Id> paths) {
                            ProductBinding b;
                            b.owner = owner;
                            b.block = block;
                            b.record = record;
                            b.kind = kind;
                            b.node_scope = node_scope;
                            if (empty_selector(paths))
                              b.status = ProductBindingStatus::empty;
                            out.product_bindings.push_back(b);
                          });
    };
    for (const auto &s : data->unparsed_core)
      missing("NWF core override unparsed: " + s);
    std::vector<Id> leaves;
    for (size_t i = owner + 1; i < end; ++i)
      if (out.nodes[i].model != none)
        leaves.push_back(static_cast<Id>(i));
    if (data->appearance_overrides.empty() &&
        data->transform_overrides.empty() && data->texture_spaces.empty() &&
        !have_product_selectors && !have_saved_paths) {
      unresolved_products();
      bind_saved_paths(owner, no_saved_target);
      return;
    }
    using Location = std::pair<Id, Id>;
    std::vector<std::vector<Location>> mapping(data->paths.size());
    std::map<Id, ModelIndex> indices;
    for (auto node : leaves) {
      indices.emplace(node, ModelIndex(model(node)));
      if (mapping.size() < 2) {
        missing("NWF overrides lack path root");
        unresolved_products();
        bind_saved_paths(owner, no_saved_target);
        if (!data->transform_overrides.empty())
          for (Id leaf : leaves)
            out.nodes[leaf].placement_supported = false;
        return;
      }
      mapping[1].emplace_back(node, 0);
    }
    for (size_t i = 2; i < data->paths.size(); ++i) {
      const auto &p = data->paths[i];
      std::vector<Location> found;
      for (auto [node, parent] : mapping.at(p.parent)) {
        const auto &m = model(node);
        auto children = indices.at(node).children(parent);
        if (p.parent == 1 && p.type == 32 && match(p, m, 0))
          found.emplace_back(node, 0);
        for (size_t j = 0; j < children.size(); ++j) {
          auto child = children[j];
          if (match(p, m, child) && (!(p.flags & 64) || p.sibling < 0 ||
                                     static_cast<size_t>(p.sibling) == j))
            found.emplace_back(node, child);
        }
      }
      if (found.size() == 1)
        mapping[i] = std::move(found);
      else
        missing("NWF path " + std::to_string(i) + " " + p.name + ": " +
                (found.empty() ? "unmatched" : "ambiguous"));
    }
    for (const auto &selection : data->appearance_overrides)
      for (auto id : selection.paths) {
        if (!(data->appearance_values.appearances.at(selection.appearance)
                  .flags &
              1)) {
          missing("NWF partial material override unsupported");
          continue;
        }
        if (id == none || id == 0)
          continue;
        require(id < mapping.size(), "NWF selection ID bounds");
        if (mapping[id].empty()) {
          missing("unresolved NWF material assignment " + std::to_string(id));
          continue;
        }
        for (auto [node, parent] : mapping[id]) {
          std::vector<Id> paths{parent};
          while (!paths.empty()) {
            auto p = paths.back();
            paths.pop_back();
            for (auto instance : indices.at(node).instances(p))
              overrides[{node, instance}] = {source_id, none,
                                             selection.appearance};
            auto children = indices.at(node).children(p);
            paths.insert(paths.end(), children.begin(), children.end());
          }
        }
      }
    auto select_node =
        [&](std::span<const Id> paths) -> std::optional<Location> {
      std::vector<std::pair<Location, size_t>> candidates;
      std::map<std::tuple<Id, Id, Id>, size_t> candidate_indices;
      for (auto id : paths) {
        if (id == none || id == 0)
          continue;
        if (id >= mapping.size() || mapping[id].size() != 1) {
          return {};
        }
        auto location = mapping[id].front();
        auto ref = path_reference(model(location.first), location.second);
        auto [same, inserted] = candidate_indices.emplace(
            std::tuple{location.first, ref.graph, ref.object},
            candidates.size());
        if (inserted)
          candidates.push_back({location, 1});
        else
          ++candidates[same->second].second;
      }
      if (candidates.empty())
        return {};
      // ReadNode chooses the most frequent shared node, retaining the first
      // candidate on ties. Keep reference occurrences in separate namespaces.
      auto best = std::max_element(
          candidates.begin(), candidates.end(),
          [](const auto &a, const auto &b) { return a.second < b.second; });
      return best->first;
    };
    for (Id record = 0; record < data->texture_spaces.size(); ++record) {
      const auto &t = data->texture_spaces[record];
      if (empty_selector(t.paths))
        continue;
      auto selected = select_node(t.paths);
      if (!selected) {
        missing("unresolved NWF texture space assignment " +
                std::to_string(record));
        continue;
      }
      const auto [node, path] = *selected;
      out.texture_space_assignments.push_back({owner, node, path, record});
    }
    if (products)
      product_selectors(*products, [&](Id block, Id record,
                                       ProductBindingKind kind, bool node_scope,
                                       std::span<const Id> paths) {
        auto &binding = out.product_bindings.emplace_back();
        binding.owner = owner;
        binding.block = block;
        binding.record = record;
        binding.kind = kind;
        binding.node_scope = node_scope;
        if (empty_selector(paths)) {
          binding.status = ProductBindingStatus::empty;
          return;
        }
        const auto &name = products->chunks[block].name;
        auto split = name.rfind('\\');
        auto prefix =
            split == name.npos ? std::string{} : name.substr(0, split);
        auto selected = prefix == data->path_map_namespace
                            ? select_node(paths)
                            : std::optional<Location>{};
        if (selected) {
          binding.status = ProductBindingStatus::resolved;
          binding.node = selected->first;
          binding.path = selected->second;
        } else
          missing("unresolved NWF product assignment " + name + " record " +
                  std::to_string(record));
      });
    bind_saved_paths(owner, [&](Id id) -> std::optional<Location> {
      if (id < mapping.size() && mapping[id].size() == 1)
        return mapping[id].front();
      return {};
    });
    if (!data->transform_overrides.empty()) {
      for (Id leaf : leaves) {
        const auto &m = model(leaf);
        const auto ref = out.nodes[leaf].parent;
        if (ref == none || out.nodes[ref].parent != owner ||
            m.linear_units != static_cast<int>(data->linear_units) ||
            std::any_of(
                m.instances.begin(), m.instances.end(),
                [](const auto &i) { return (i.flags & 0x20000000) != 0; })) {
          missing("NWF object transforms require a direct same-unit source "
                  "without baked overrides");
          out.nodes[leaf].placement_supported = false;
        }
      }
      for (const auto &t : data->transform_overrides) {
        if (t.path == 0 || t.path == none)
          continue;
        require(t.path < mapping.size(), "transform path bounds");
        if (mapping[t.path].size() != 1) {
          missing("unresolved NWF object transform assignment " +
                  std::to_string(t.path));
          for (Id leaf : leaves)
            out.nodes[leaf].placement_supported = false;
          continue;
        }
        const auto [leaf, path] = mapping[t.path].front();
        // Native snapshot records name geometry nodes, not group subtrees.
        if (path_object(model(leaf), path).type != 22) {
          missing("NWF transform assignment is not a geometry node");
          out.nodes[leaf].placement_supported = false;
          continue;
        }
        auto matrix = nwf_transform_matrix(t);
        auto scale = unit(data->linear_units);
        for (unsigned i = 0; i < 3; ++i) {
          matrix[12 + i] *= scale;
          matrix[i * 4 + 3] /= scale;
        }
        for (Id instance : indices.at(leaf).instances(path))
          transform_overrides[{owner, leaf, instance}] = matrix;
      }
    }
  }
  void visit(const std::filesystem::path &file, Id node, unsigned depth,
             const std::string &partition = {}) {
    if (depth > options.max_reference_depth) {
      out.nodes[node].status = "depth_limit";
      missing("reference depth exceeded: " + utf8(file));
      return;
    }
    auto k = key(file);
    if (active.contains(k)) {
      out.nodes[node].status = "cycle";
      missing("cyclic reference: " + utf8(file));
      return;
    }
    active.insert(k);
    struct End {
      std::set<std::string> &a;
      std::string k;
      ~End() { a.erase(k); }
    } end{active, k};
    Id sid;
    try {
      sid = source(file);
    } catch (const std::exception &e) {
      out.nodes[node].status = "parse_error";
      missing(utf8(file) + ": " + e.what());
      return;
    }
    out.nodes[node].source = sid;
    out.nodes[node].status = "loaded";
    auto scene = out.sources[sid].scene;
    auto nwf = out.sources[sid].nwf;
    if (scene) {
      for (const auto &w : scene->warnings)
        out.warnings.push_back(utf8(file) + ": " + w);
      bool found = false;
      for (size_t i = 0; i < scene->models.size(); ++i) {
        const auto &m = scene->models[i];
        if (!partition.empty() && m.name != partition)
          continue;
        found = true;
        ProjectNode n;
        n.parent = node;
        n.source = sid;
        n.model = static_cast<Id>(i);
        n.name = m.name;
        n.status = "loaded";
        out.nodes.push_back(std::move(n));
        for (const auto &g : m.geometries)
          if (g.external) {
            missing("external point/mesh coordinates unavailable: " +
                    g.external->source_path);
            break;
          }
      }
      if (!found)
        missing("referenced partition not found: " + partition + " in " +
                utf8(file));
      return;
    }
    for (size_t i = 0; i < nwf->references.size(); ++i) {
      const auto &r = nwf->references[i];
      ProjectNode n;
      n.parent = node;
      n.reference = static_cast<Id>(i);
      n.name = r.display_name.empty() ? r.name : r.display_name;
      n.requested_path = r.original_path;
      auto resolved = resolve(file, r.original_path, nwf->path_remaps);
      auto child = static_cast<Id>(out.nodes.size());
      n.status = resolved.status;
      out.nodes.push_back(std::move(n));
      if (resolved.status != "resolved") {
        missing("reference " + resolved.status + ": " + r.original_path);
        continue;
      }
      for (const auto &c : r.cached_files)
        if (c.size &&
            path(c.name).filename() == path(r.original_path).filename()) {
          std::error_code ec;
          auto size = std::filesystem::file_size(resolved.file, ec);
          if (!ec && size != c.size)
            out.warnings.push_back("referenced file differs from saved size: " +
                                   utf8(resolved.file));
        }
      visit(resolved.file, child, depth + 1, r.partition);
      auto csid = out.nodes[child].source;
      if (csid != none) {
        const auto &cs = out.sources[csid];
        try {
          if (cs.scene) {
            Id leaf = none;
            for (Id j = child + 1; j < out.nodes.size(); ++j)
              if (out.nodes[j].parent == child && out.nodes[j].model != none) {
                require(leaf == none,
                        "aggregate NWF reference placement not supported");
                leaf = j;
              }
            require(leaf != none, "reference has no model partition");
            const auto &m = cs.scene->models[out.nodes[leaf].model];
            out.nodes[leaf].to_parent =
                reference_placement_matrix(m, r, nwf->linear_units);
            auto base = base_attribute(m);
            bool old_orientation = base && base->type == 39 &&
                                   !base->integers.empty() &&
                                   base->integers[0] != 0;
            out.nodes[leaf].orientation_changed =
                old_orientation != (r.orientation_flag != 0);
          } else {
            require(cs.nwf && affine(r.affine) == identity() &&
                        r.linear_units == cs.nwf->linear_units &&
                        r.orientation_flag == 0,
                    "nested NWF placement override not supported");
          }
        } catch (const Error &e) {
          out.nodes[child].placement_supported = false;
          missing(r.name + ": " + e.what());
        }
      }
    }
    bind(node, out.nodes.size());
  }
  void textures() {
    using AssetKey = std::tuple<Id, Id, Id>;
    std::set<AssetKey> active_assets;
    for (Id node = 0; node < out.nodes.size(); ++node) {
      const auto &n = out.nodes[node];
      if (n.model == none)
        continue;
      const auto &m = model(node);
      std::set<std::tuple<Id, Id, Id>> appearances;
      std::vector<uint8_t> used(m.appearances.size());
      auto it = overrides.lower_bound({node, 0});
      for (Id i = 0; i < m.instances.size(); ++i) {
        if (it != overrides.end() && it->first == std::pair{node, i}) {
          const auto &ref = it->second;
          if (ref.appearance != none)
            appearances.emplace(ref.source, ref.model, ref.appearance);
          ++it;
        } else if (m.instances[i].appearance != none)
          used.at(m.instances[i].appearance) = 1;
      }
      for (Id i = 0; i < used.size(); ++i)
        if (used[i])
          appearances.emplace(n.source, n.model, i);
      for (auto [sid, mid, aid] : appearances) {
        const auto &arena = appearance_arena(out, {sid, mid, aid});
        auto asset = arena.appearances.at(aid).asset;
        if (asset != none)
          active_assets.emplace(sid, mid, asset);
      }
    }
    auto collect = [&](Id sid, Id mid, const Model &arena) {
      auto &s = out.sources.at(sid);
      for (Id aid = 0; aid < arena.assets.size(); ++aid) {
        const auto &a = arena.assets[aid];
        bool active = active_assets.contains({sid, mid, aid});
        AssetDescription desc;
        try {
          desc = describe_asset(a);
        } catch (const Error &e) {
          if (active)
            missing(e.what());
        }
        auto preview = [&](const std::string &alias) {
          bool found = false;
          for (const auto &u : desc.uris)
            if (u.value == alias) {
              if (!u.thumbnail)
                return false;
              found = true;
            }
          return found;
        };
        std::vector<std::pair<std::string, std::string>> files = a.files;
        // URI-only resources also need an explicit availability result.
        for (const auto &u : desc.uris)
          if (!u.thumbnail && !u.value.empty() &&
              std::none_of(files.begin(), files.end(),
                           [&](const auto &f) { return f.first == u.value; }))
            files.emplace_back(u.value, u.value);
        for (const auto &f : a.embedded_files) {
          TextureFile t;
          t.source = sid;
          t.model = mid;
          t.asset = aid;
          t.alias = f.name;
          t.active = active;
          t.thumbnail = preview(f.name);
          t.status = "embedded";
          if (s.scene)
            t.bytes = {s.scene, &f.bytes};
          else
            t.bytes = {s.nwf, &f.bytes};
          out.textures.push_back(std::move(t));
        }
        for (const auto &[alias, stored] : files) {
          TextureFile t;
          t.source = sid;
          t.model = mid;
          t.asset = aid;
          t.alias = alias;
          t.active = active;
          t.thumbnail = preview(alias);
          t.requested_path = stored.empty() ? alias : stored;
          const auto &requested = t.requested_path;
          auto embedded =
              std::find_if(a.embedded_files.begin(), a.embedded_files.end(),
                           [&](const auto &f) {
                             return f.name == requested || f.name == alias;
                           });
          if (embedded != a.embedded_files.end()) {
            t.status = "embedded_alias";
            if (s.scene)
              t.bytes = {s.scene, &embedded->bytes};
            else
              t.bytes = {s.nwf, &embedded->bytes};
          } else if (s.scene && requested.starts_with("nwd:")) {
            auto it = std::find_if(
                s.scene->resources.begin(), s.scene->resources.end(),
                [&](const auto &f) { return f.name == requested; });
            if (it != s.scene->resources.end()) {
              t.status = "embedded_resource";
              t.bytes = {s.scene, &it->bytes};
            }
          }
          if (!t.bytes) {
            auto r = resolve(
                s.path, requested,
                s.nwf ? s.nwf->path_remaps
                      : std::vector<std::pair<std::string, std::string>>{});
            t.status = r.status;
            t.resolved_path = r.file;
            if (r.status == "resolved") {
              auto k = key(r.file);
              auto it = texture_cache.find(k);
              if (it == texture_cache.end()) {
                auto size = std::filesystem::file_size(r.file);
                require(size <= options.reader.max_decoded_chunk,
                        "texture file resource limit");
                auto bytes = std::make_shared<std::vector<uint8_t>>(
                    static_cast<size_t>(size));
                std::ifstream f(r.file, std::ios::binary);
                require(bool(f.read(reinterpret_cast<char *>(bytes->data()),
                                    static_cast<std::streamsize>(size))),
                        "texture read error");
                it = texture_cache.emplace(k, bytes).first;
              }
              t.bytes = it->second;
            } else if (t.active && !t.thumbnail)
              missing("texture " + r.status + ": " + requested);
          }
          out.textures.push_back(std::move(t));
        }
      }
    };
    for (Id sid = 0; sid < out.sources.size(); ++sid) {
      const auto &s = out.sources[sid];
      if (s.nwf)
        collect(sid, none, s.nwf->appearance_values);
      else
        for (Id mid = 0; mid < s.scene->models.size(); ++mid)
          collect(sid, mid, s.scene->models[mid]);
    }
  }

public:
  explicit Loader(ProjectOptions o) : options(std::move(o)) {
    require(options.reader.metadata, "project import requires metadata");
    if (options.load_textures)
      options.reader.resources = true;
  }
  Project run(const std::filesystem::path &input) {
    ProjectNode root;
    root.name = utf8(input.filename());
    out.nodes.push_back(root);
    std::error_code ec;
    auto f = std::filesystem::canonical(input, ec);
    if (ec) {
      out.nodes[0].status = "missing";
      missing("input missing: " + utf8(input));
      return std::move(out);
    }
    visit(f, 0, 0);
    if (options.load_textures)
      textures();
    for (const auto &[key, value] : overrides)
      out.appearance_overrides.push_back({key.first, key.second, value});
    for (const auto &[key, value] : transform_overrides) {
      const auto [owner, node, instance] = key;
      out.transform_overrides.push_back({owner, node, instance, value});
    }
    std::sort(out.saved_path_bindings.begin(), out.saved_path_bindings.end(),
              [](const auto &a, const auto &b) {
                return std::tie(a.owner, a.block, a.path_link) <
                       std::tie(b.owner, b.block, b.path_link);
              });
    return std::move(out);
  }
};
} // namespace
Project load_project(const std::filesystem::path &path,
                     ProjectOptions options) {
  return Loader(std::move(options)).run(path);
}
const SavedPathBinding *saved_path_binding(const Project &project, Id owner,
                                           Id block, Id path_link) {
  auto key = std::tuple{owner, block, path_link};
  auto it = std::lower_bound(
      project.saved_path_bindings.begin(), project.saved_path_bindings.end(),
      key, [](const auto &entry, const auto &key) {
        return std::tie(entry.owner, entry.block, entry.path_link) < key;
      });
  if (it == project.saved_path_bindings.end() ||
      std::tie(it->owner, it->block, it->path_link) != key)
    return nullptr;
  return &*it;
}
Matrix nwf_transform_matrix(const NwfTransformOverride &t) {
  const size_t count = !t.flags           ? 0
                       : (t.flags & 32)   ? 16
                       : !(t.flags & ~7u) ? 9
                       : t.flags == 16    ? 3
                                          : 12;
  detail::require(t.values.size() == count, "NWF transform payload size");
  for (double v : t.values)
    detail::require(std::isfinite(v), "nonfinite NWF transform");
  if (count == 12)
    return affine(t.values);
  auto matrix = identity();
  if (count == 16)
    std::copy(t.values.begin(), t.values.end(), matrix.begin());
  else if (count == 9)
    for (unsigned col = 0; col < 3; ++col)
      for (unsigned row = 0; row < 3; ++row)
        matrix[col * 4 + row] = t.values[col * 3 + row];
  else if (count == 3)
    for (unsigned i = 0; i < 3; ++i)
      matrix[12 + i] = t.values[i];
  return matrix;
}
Matrix model_base_matrix(const Model &m) {
  auto o = base_attribute(m);
  if (!o || o->type == 38)
    return identity();
  if (o->type == 39)
    return affine(o->numbers);
  detail::require(o->numbers.size() == (o->type == 40 ? 3u : 7u),
                  "invalid source base transform");
  Model temporary;
  Transform t;
  t.type = o->type == 40 ? 14 : 15;
  std::copy(o->numbers.begin(), o->numbers.end(), t.values.begin());
  for (double v : o->numbers)
    detail::require(std::isfinite(v), "nonfinite source base transform");
  temporary.transforms.push_back(t);
  Instance i;
  i.transform = 0;
  return world_matrix(temporary, i);
}
Matrix reference_placement_matrix(const Model &m, const NwfReference &r,
                                  uint32_t parent_units) {
  detail::require(m.linear_units >= 0 &&
                      r.linear_units == static_cast<uint32_t>(m.linear_units),
                  "NWF source unit override not supported");
  detail::require(r.orientation_flag <= 1, "invalid NWF orientation hint");
  auto old = model_base_matrix(m), next = affine(r.affine);
  // An unchanged base needs no conversion even with a different parent unit.
  if (old == next)
    return identity();
  detail::require(parent_units == r.linear_units,
                  "NWF mixed-unit placement override not supported");
  auto delta = multiply(next, inverse(old));
  for (unsigned i = 0; i < 3; ++i)
    delta[12 + i] *= unit(r.linear_units);
  return delta;
}
const Model &appearance_arena(const Project &p, ProjectAppearance a) {
  const auto &s = p.sources.at(a.source);
  if (a.model == none) {
    detail::require(bool(s.nwf), "appearance arena is not NWF");
    return s.nwf->appearance_values;
  }
  detail::require(bool(s.scene), "appearance arena is not scene");
  return s.scene->models.at(a.model);
}
ProjectAppearance effective_appearance(const Project &p, Id node, Id instance) {
  const auto &n = p.nodes.at(node);
  const auto &source = p.sources.at(n.source);
  detail::require(bool(source.scene) && n.model != none,
                  "project node is not a model");
  const auto &m = source.scene->models.at(n.model);
  detail::require(instance < m.instances.size(), "instance bounds");
  auto it = std::lower_bound(
      p.appearance_overrides.begin(), p.appearance_overrides.end(),
      std::pair{node, instance}, [](const AppliedAppearance &a, const auto &b) {
        return std::pair{a.node, a.instance} < b;
      });
  if (it != p.appearance_overrides.end() && it->node == node &&
      it->instance == instance)
    return it->appearance;
  return {n.source, n.model, m.instances[instance].appearance};
}
Matrix project_world_matrix(const Project &p, Id node, Id instance) {
  const auto &n = p.nodes.at(node);
  const auto &source = p.sources.at(n.source);
  detail::require(bool(source.scene) && n.model != none,
                  "project node is not a model");
  const auto &m = source.scene->models.at(n.model);
  auto mat = world_matrix(m, m.instances.at(instance));
  detail::require(m.meters_per_unit > 0, "unknown model units");
  for (unsigned col = 0; col < 4; ++col)
    for (unsigned row = 0; row < 3; ++row)
      mat[col * 4 + row] *= m.meters_per_unit;
  for (Id id = node; id != none; id = p.nodes.at(id).parent) {
    detail::require(p.nodes[id].parent == none || p.nodes[id].parent < id,
                    "project parent cycle");
    detail::require(p.nodes[id].placement_supported,
                    "unvalidated NWF placement; inspect raw NwfReference");
    const auto key = std::tuple{id, node, instance};
    auto it = std::lower_bound(
        p.transform_overrides.begin(), p.transform_overrides.end(), key,
        [](const AppliedTransform &a, const auto &key) {
          return std::tuple{a.owner, a.node, a.instance} < key;
        });
    if (it != p.transform_overrides.end() &&
        std::tuple{it->owner, it->node, it->instance} == key)
      mat = multiply(it->matrix, mat);
    mat = multiply(p.nodes[id].to_parent, mat);
  }
  return mat;
}
} // namespace nwd
