#pragma once
#include "internal.hpp"

namespace nwd::detail {
// Numeric payload shared by run-transform objects. Object framing and stream
// identity remain with the caller; do not intern identities in this helper.
inline Transform read_run_transform_fields(Cursor &r, uint32_t type,
                                           uint32_t version = 448) {
  require(type >= 14 && type <= 17, "run transform type");
  Transform t;
  t.type = type;
  if (type == 17 && version == 0) {
    for (unsigned i = 0; i < 12; ++i)
      t.values[i] = r.f64();
    // The old sixteen-double layout stores XYZ before the homogeneous scalar.
    // Keep double precision, but expose the same field order as later matrices.
    for (unsigned i = 13; i < 16; ++i)
      t.values[i] = r.f64();
    t.values[12] = r.f64();
  } else if (type == 17) {
    for (unsigned i = 0; i < 13; ++i)
      t.values[i] = r.f32();
    for (unsigned i = 13; i < 16; ++i)
      t.values[i] = r.f64();
  } else {
    const unsigned n = type == 14 ? 3 : type == 15 ? 7 : 8;
    for (unsigned i = 0; i < n; ++i)
      t.values[i] = r.f64();
  }
  return t;
}
// GeomRef::ReadUserContents only. Inline geometry versus a geometry table is
// selected by the enclosing stream context and must be handled separately.
inline GeometryReference read_geometry_reference_user(Cursor &r,
                                                      uint32_t version) {
  GeometryReference g;
  if (version < 2)
    return g; // no user fields are serialized; no generated runtime defaults
  for (auto &v : g.bounds)
    v = r.f32();
  for (auto &v : g.origin)
    v = r.f64();
  g.tolerance = r.f32();
  if (version >= 5)
    g.checksum = r.u32();
  return g;
}
inline AuxiliaryTransform read_auxiliary_transform_fields(Cursor &r) {
  AuxiliaryTransform saved;
  saved.type = r.u32();
  require(saved.type <= 2, "unknown auxiliary shape transform type");
  const unsigned n = saved.type == 0 ? 12 : saved.type == 1 ? 3 : 7;
  for (unsigned i = 0; i < n; ++i)
    saved.values[i] = r.f64();
  if (saved.type == 0) {
    auto flag = r.u32();
    require(flag <= 1, "invalid auxiliary transform orientation");
    saved.orientation = flag != 0;
  }
  return saved;
}
} // namespace nwd::detail
