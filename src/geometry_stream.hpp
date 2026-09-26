#pragma once
#include "internal.hpp"

namespace nwd::detail {
// Packed fields are shared by normals, colors, UVs and quantized coordinates.
inline uint32_t packed_bits(std::span<const uint8_t> bytes, uint64_t bit,
                            unsigned width) {
  require(width <= 32, "packed component bit width");
  if (!width)
    return 0;
  uint64_t w = bit / 32;
  unsigned shift = bit % 32;
  size_t need = shift + width > 32 ? 8 : 4;
  require(w <= bytes.size() / 4 && need <= bytes.size() - w * 4,
          "truncated packed palette");
  uint32_t first;
  std::memcpy(&first, bytes.data() + w * 4, 4);
  uint64_t value = first;
  if (need == 8) {
    uint32_t second;
    std::memcpy(&second, bytes.data() + w * 4 + 4, 4);
    value = ((value << 32) | second) >> (64 - shift - width);
  } else
    value >>= 32 - shift - width;
  return static_cast<uint32_t>(
      value & (width == 32 ? UINT32_MAX : (1ull << width) - 1));
}

struct CoordinatePayload {
  std::vector<float> values;
  std::vector<uint32_t> indices;
  std::shared_ptr<const CoordinateQuantization> quantization;
};
CoordinatePayload read_coordinate_payload(Cursor &, bool raw,
                                          uint64_t max_entries);
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
// Payload only: the enclosing arena owns the wire object ID and sharing.
AttributeArray read_geometry_attribute_payload(Cursor &, uint32_t type,
                                               uint32_t vertex_count,
                                               const GeometryStreamContext &);
Geometry decode_geometry_record(std::span<const uint8_t>,
                                const GeometryStreamContext &,
                                unsigned normal_override = 0,
                                uint64_t max_entries = 100000000);
} // namespace nwd::detail
