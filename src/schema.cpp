#include "internal.hpp"
namespace nwd {
namespace {
using namespace detail;
uint32_t count(Cursor &r) {
  auto n = r.u32();
  require(n <= 1000000, "schema count limit");
  return n;
}
std::vector<std::string> strings(Cursor &r) {
  std::vector<std::string> out;
  for (auto n = count(r); n; --n)
    out.push_back(r.string());
  return out;
}
void read_guid(Cursor &r, std::array<uint8_t, 16> &out) {
  r.align(4);
  auto b = r.raw(16);
  std::copy(b.begin(), b.end(), out.begin());
}
SchemaValue scalar(Cursor &r, uint32_t type) {
  SchemaValue v;
  v.type = type;
  switch (type) {
  case 0:
    v.number = r.read<double>();
    break;
  case 1:
    read_guid(r, v.guid);
    break;
  case 2:
    v.integer = r.read<int32_t>();
    break;
  case 3:
    v.integer = r.read<int32_t>();
    require(v.integer == 0 || v.integer == 1, "schema boolean value");
    break;
  case 4:
    v.text = r.string();
    break;
  default:
    r.fail("unknown schema scalar type");
  }
  return v;
}
SchemaField field(Cursor &r, uint32_t type, uint32_t version, unsigned depth,
                  size_t &budget) {
  require(depth < 128 && budget > 0, "schema field resource limit");
  --budget;
  SchemaField f;
  f.type = type;
  f.name = r.string();
  if (version < 419)
    f.display_name = r.string();
  if (version >= 413)
    f.concepts = strings(r);
  if (version >= 419)
    f.display_name = r.string();
  f.default_value.type = type;
  switch (type) {
  case 0:
    if (version >= 418) {
      f.default_value = scalar(r, type);
      f.qualifier = r.u32();
    }
    break;
  case 1:
    f.qualifier = r.u32();
    f.qualifier_name = r.string();
    if (version >= 418)
      f.default_value = scalar(r, type);
    break;
  case 2:
  case 3:
  case 4:
    if (version >= 418)
      f.default_value = scalar(r, type);
    break;
  case 20:
    for (auto n = count(r); n; --n) {
      auto t = r.u32();
      f.children.push_back(field(r, t, version, depth + 1, budget));
    }
    break;
  case 21: {
    auto t = r.u32();
    f.children.push_back(field(r, t, version, depth + 1, budget));
    break;
  }
  default:
    r.fail("unsupported schema field type " + std::to_string(type));
  }
  return f;
}
SchemaValue value(Cursor &r, const SchemaField &f, unsigned depth,
                  size_t &budget) {
  require(depth < 128 && budget > 0, "schema instance resource limit");
  --budget;
  if (f.type <= 4)
    return scalar(r, f.type);
  SchemaValue v;
  v.type = f.type;
  if (f.type == 20) {
    for (const auto &child : f.children)
      v.children.push_back(value(r, child, depth + 1, budget));
  } else if (f.type == 21) {
    require(f.children.size() == 1, "schema vector element definition");
    for (auto n = count(r); n; --n)
      v.children.push_back(value(r, f.children[0], depth + 1, budget));
  } else
    r.fail("unsupported schema value type");
  return v;
}
} // namespace
std::vector<SchemaDefinition> decode_schemas(std::span<const uint8_t> b,
                                             uint32_t version) {
  detail::Cursor r(b, "common schemas");
  std::vector<SchemaDefinition> out;
  size_t budget = 1000000;
  for (auto n = count(r); n; --n) {
    SchemaDefinition s;
    s.names = strings(r);
    s.root = field(r, 20, version, 0, budget);
    out.push_back(std::move(s));
  }
  r.exact();
  return out;
}
namespace detail {
SchemaInstance read_schema_instance(Cursor &r,
                                    std::span<const SchemaDefinition> schemas) {
  SchemaInstance instance;
  auto id = r.u32();
  require(id <= schemas.size(), "schema instance reference outside table");
  if (id) {
    instance.schema = id - 1;
    size_t budget = 1000000;
    instance.value = value(r, schemas[instance.schema].root, 0, budget);
  }
  return instance;
}
void decode_external_payload(ExternalGeometry &e,
                             std::span<const SchemaDefinition> schemas,
                             uint32_t version) {
  // Align relative to the original geometry record, not to the saved tail.
  size_t prefix = e.payload_record_offset % 8;
  Bytes b(prefix, 0);
  b.insert(b.end(), e.unparsed_payload.begin(), e.unparsed_payload.end());
  Cursor r(b, "external geometry payload");
  r.pos = prefix;
  if (version >= 434) {
    auto id = r.u32();
    require(id <= schemas.size(), "external geometry schema reference");
    if (id) {
      e.schema = id - 1;
      size_t budget = 1000000;
      e.properties = value(r, schemas[e.schema].root, 0, budget);
    }
  }
  e.flags = r.u32();
  for (auto &v : e.bounds)
    v = r.f32();
  e.flags2 = r.u32();
  e.geometry_kind = r.u32();
  r.exact();
  e.payload_decoded = true;
}
} // namespace detail
} // namespace nwd
