#include "internal.hpp"
#include <cmath>
#include <map>
namespace nwd {
namespace {
using Name = std::pair<std::string, std::string>;
using Key = std::pair<Id, Id>;
Name property_name(const ObjectGraph &g, Id id) {
  detail::require(id < g.objects.size(), "property index name outside graph");
  const auto &o = g.objects[id];
  detail::require((o.type == 52 || o.type == 82) && o.strings.size() == 2,
                  "property index name type");
  // Equal distinguishes null names from allocated empty names. Keep that
  // distinction in the identity key while the first field remains the label.
  std::string internal(1, static_cast<char>((o.strings[0] == none ? 1 : 0) |
                                            (o.strings[1] == none ? 2 : 0)));
  internal.append(resolve_string(g, o.strings[1]));
  return {std::string(resolve_string(g, o.strings[0])), std::move(internal)};
}
std::optional<std::string> legacy_label(const ObjectGraph &g, const Value &v) {
  if (v.tag != 4)
    return {};
  auto s = resolve_string(g, v.string);
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r') {
      if (i + 1 < s.size() && s[i + 1] == '\n')
        ++i;
      out.push_back(' ');
    } else
      out.push_back(s[i] == '\n' ? ' ' : s[i]);
  }
  return out;
}
// A display collision must not merge distinct typed source values.
std::optional<std::string> value_key(const ObjectGraph &g, const Value &v) {
  std::string key;
  auto put = [&](const auto &x) {
    key.append(reinterpret_cast<const char *>(&x), sizeof(x));
  };
  put(v.tag);
  switch (v.tag) {
  case 0:
    break;
  case 2:
  case 3:
  case 5:
  case 14:
  case 15:
  case 16:
    put(v.integer);
    break;
  case 4:
  case 9:
    put(v.string == none);
    key.append(resolve_string(g, v.string));
    break;
  case 8:
    put(v.reference.graph);
    put(v.reference.object);
    break;
  default:
    detail::require(v.tag == 1 || v.tag == 6 || v.tag == 7 || v.tag == 10 ||
                        v.tag == 11 || v.tag == 12 || v.tag == 13,
                    "property index unknown value type");
    for (unsigned i = 0, n = v.tag == 12   ? 3
                             : v.tag == 13 ? 2
                                           : 1;
         i < n; ++i) {
      double x = v.number[i] == 0 ? 0 : v.number[i];
      if (!std::isfinite(x))
        return {};
      put(x);
    }
  }
  return key;
}
} // namespace
struct PropertyLocatorIndex::Impl {
  struct Values {
    bool ambiguous = false;
    std::string key;
    std::vector<PropertyReference> records;
  };
  struct PropertyGroup {
    std::vector<PropertyReference> records;
    std::map<std::string, Values, std::less<>> values;
    bool incomplete_labels = false;
    bool unsupported_values = false;
  };
  struct Category {
    std::vector<PropertyReference> records;
    std::map<Name, PropertyGroup> properties;
  };
  std::map<Key, std::vector<Id>> owners;
  std::map<Name, Category> categories;
};
PropertyLocatorIndex::PropertyLocatorIndex(const Model &m,
                                           PropertyIndexOptions options) {
  auto out = std::make_shared<Impl>();
  uint64_t remaining = options.max_entries;
  uint64_t remaining_bytes = options.max_label_bytes;
  auto charge_bytes = [&](size_t size) {
    detail::require(size <= remaining_bytes, "property index label byte limit");
    remaining_bytes -= size;
  };
  auto charge_name = [&](const Name &name) {
    charge_bytes(name.first.size());
    charge_bytes(name.second.size());
  };
  auto charge = [&] {
    detail::require(remaining > 0, "property index resource limit");
    --remaining;
  };
  detail::require(m.paths.size() < none, "property index path limit");
  for (Id path = 0; path < m.paths.size(); ++path) {
    const auto &node = path_object(m, path);
    for (Reference ref : node.attributes) {
      if (ref.object == none)
        continue;
      const auto &attribute = resolve_object(m, ref);
      if (attribute.type != 84 && attribute.type != 86)
        continue;
      if (!options.include_internal && (attribute.flags & 0x20000))
        continue;
      if (attribute.class_name == none)
        continue;
      auto &owners = out->owners[{ref.graph, ref.object}];
      if (owners.empty() || owners.back() != path) {
        charge();
        owners.push_back(path);
      }
    }
  }
  for (const auto &[id, paths] : out->owners) {
    Reference ref{id.first, id.second};
    const auto &g = m.graphs[ref.graph];
    const auto &a = g.objects[ref.object];
    auto category_name = property_name(g, a.class_name);
    if (out->categories.find(category_name) == out->categories.end())
      charge_name(category_name);
    auto &category = out->categories[std::move(category_name)];
    charge();
    category.records.push_back({ref, none});
    detail::require(a.properties.size() < none,
                    "property index property limit");
    for (Id i = 0; i < a.properties.size(); ++i) {
      const auto &p = a.properties[i];
      if (p.name == none)
        continue;
      PropertyReference source{ref, i};
      auto name = property_name(g, p.name);
      if (category.properties.find(name) == category.properties.end())
        charge_name(name);
      auto &property = category.properties[std::move(name)];
      charge();
      property.records.push_back(source);
      auto label = options.format_value ? options.format_value(m, source)
                                        : legacy_label(g, p.value);
      if (!label) {
        property.incomplete_labels = true;
        continue;
      }
      auto key = value_key(g, p.value);
      if (!key) {
        property.unsupported_values = true;
        continue;
      }
      if (property.values.find(*label) == property.values.end()) {
        charge_bytes(label->size());
        charge_bytes(key->size());
      }
      auto [value, fresh] = property.values.try_emplace(std::move(*label));
      if (fresh)
        value->second.key = std::move(*key);
      else if (value->second.key != *key)
        value->second.ambiguous = true;
      charge();
      value->second.records.push_back(source);
    }
  }
  impl_ = std::move(out);
}
PropertyLocatorResult
PropertyLocatorIndex::resolve(const SelectionLocatorPath &path) const {
  using Status = PropertyLocatorStatus;
  if (path.root != "lcop_property_tree")
    return {Status::unsupported_root, {}};
  if (path.parts.empty())
    return {Status::tree_root, {}};
  if (path.parts.size() > 3)
    return {Status::unsupported_path, {}};
  auto category = impl_->categories.lower_bound({path.parts[0], {}});
  if (category == impl_->categories.end() ||
      category->first.first != path.parts[0])
    return {};
  auto next = std::next(category);
  if (next != impl_->categories.end() && next->first.first == path.parts[0])
    return {Status::ambiguous, {}};
  if (path.parts.size() == 1)
    return {Status::resolved, category->second.records};
  const auto &properties = category->second.properties;
  auto property = properties.lower_bound({path.parts[1], {}});
  if (property == properties.end() || property->first.first != path.parts[1])
    return {};
  auto after = std::next(property);
  if (after != properties.end() && after->first.first == path.parts[1])
    return {Status::ambiguous, {}};
  if (path.parts.size() == 2)
    return {Status::resolved, property->second.records};
  if (property->second.unsupported_values)
    return {Status::unsupported_value, {}};
  if (property->second.incomplete_labels)
    return {Status::display_context_required, {}};
  auto value = property->second.values.find(path.parts[2]);
  if (value == property->second.values.end())
    return {};
  if (value->second.ambiguous)
    return {Status::ambiguous, {}};
  return {Status::resolved, value->second.records};
}
std::span<const Id> PropertyLocatorIndex::owners(Reference attribute) const {
  auto found = impl_->owners.find({attribute.graph, attribute.object});
  if (found == impl_->owners.end())
    return {};
  return found->second;
}
} // namespace nwd
