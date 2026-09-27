#pragma once
#include "geometry_stream.hpp"

namespace nwd::detail {
inline LegacyMaterialObject read_legacy_material(Cursor &r, Id owner) {
  LegacyMaterialObject value;
  value.owner = owner;
  value.saved_prefix = r.f32();
  for (unsigned i = 0; i < 12; ++i)
    value.fields.values[i] = r.f32();
  // Three scalar Int16 reads each require two-byte alignment, not four.
  for (auto &mode : value.saved_modes) {
    r.align(2);
    auto bytes = r.raw(2);
    std::memcpy(&mode, bytes.data(), 2);
  }
  value.fields.values[12] = r.f32();
  value.fields.values[13] = r.f32();
  return value;
}
inline AttributeArray read_legacy_float_attribute(Cursor &r, uint32_t type,
                                                  uint64_t limit) {
  require(type == 59 || type == 61 || type == 56 || type == 57,
          "legacy float attribute type");
  const auto n = r.read<int32_t>();
  require(n >= 0 && static_cast<uint64_t>(n) <= limit && n <= 100000000,
          "legacy float attribute count limit");
  const unsigned components = type == 61 ? 2 : type == 57 ? 4 : 3;
  auto bytes = r.raw(uint64_t(n) * components * sizeof(float));
  AttributeArray a;
  a.type = type;
  a.raw = true;
  a.bits = 32;
  a.palette_count = static_cast<uint32_t>(n);
  a.packed_palette.assign(bytes.begin(), bytes.end());
  for (size_t i = 0; i < bytes.size(); i += sizeof(float)) {
    float v;
    std::memcpy(&v, bytes.data() + i, sizeof(float));
    a.finite = a.finite && std::isfinite(v);
  }
  a.indices.resize(n);
  for (int32_t i = 0; i < n; ++i)
    a.indices[i] = i;
  return a;
}
} // namespace nwd::detail
