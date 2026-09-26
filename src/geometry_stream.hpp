#pragma once
#include "internal.hpp"

namespace nwd::detail {
// Values belong to one source stream/model scope. Presence of a geometry chunk
// selects paging; the wire version alone does not select it.
struct GeometryStreamContext {
  uint32_t version = 0;
  GeometryCompression compression;
  bool paged = false;
  bool reference_uses_store() const { return version >= 25 && paged; }
};

inline GeometryCompression read_geometry_compression(Cursor &r,
                                                     uint32_t version) {
  GeometryCompression out;
  out.flags = r.u32();
  if (version >= 26) {
    out.normal_precision = r.byte();
    out.color_precision = r.byte();
    out.texture_coordinate_precision = r.byte();
    (void)r.byte(); // native reserved byte, not a precision field
    out.coordinate_precision = r.read<float>();
  }
  return out;
}

struct GeometryStripPayload {
  std::vector<uint32_t> lengths;
  std::vector<uint32_t> indices;
  // No serialized indices before version20, for lines, or for an empty index
  // array. Keep that fact separate from the derived sequential vertex list.
  bool implicit_indices = true;
};

GeometryStripPayload read_geometry_strips(Cursor &, uint32_t type,
                                          const GeometryStreamContext &,
                                          uint32_t vertex_count,
                                          uint64_t max_entries);
} // namespace nwd::detail
