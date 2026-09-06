#include "internal.hpp"
namespace nwd {
namespace {
uint32_t bits_at(const std::vector<uint8_t> &bytes, uint64_t bit,
                 unsigned width) {
  detail::require(width <= 32, "attribute bit width");
  if (!width)
    return 0;
  uint64_t w = bit / 32;
  unsigned shift = bit % 32;
  size_t need = shift + width > 32 ? 8 : 4;
  detail::require(w * 4 <= bytes.size() && need <= bytes.size() - w * 4,
                  "truncated attribute palette");
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
} // namespace
std::array<float, 4> attribute_value(const AttributeArray &a, size_t slot) {
  using detail::require;
  require(slot < a.indices.size(), "attribute slot outside array");
  bool normal = a.type == 59 || a.type == 100;
  require(normal || a.type == 58 || a.type == 61, "unsupported attribute type");
  auto index = a.indices[slot];
  std::array<float, 4> out{};
  if (index < 0) {
    require(normal && index >= -6, "invalid attribute axis reference");
    unsigned axis = static_cast<unsigned>(-index - 1);
    out[axis / 2] = axis % 2 ? -1.f : 1.f;
    return out;
  }
  require(static_cast<uint32_t>(index) < a.palette_count,
          "attribute index outside palette");
  unsigned components = normal ? 3 : a.type == 58 ? 4 : 2;
  if (a.raw) {
    unsigned bytes_per_component = a.type == 100 ? 2 : a.type == 58 ? 1 : 4;
    size_t offset = uint64_t(index) * components * bytes_per_component;
    require(offset <= a.packed_palette.size() &&
                components * bytes_per_component <=
                    a.packed_palette.size() - offset,
            "truncated raw attribute");
    for (unsigned j = 0; j < components; ++j) {
      auto p = a.packed_palette.data() + offset + j * bytes_per_component;
      if (a.type == 100) {
        int16_t v;
        std::memcpy(&v, p, 2);
        out[j] = v / 32767.f;
      } else if (a.type == 58)
        out[j] = *p / 255.f;
      else
        std::memcpy(&out[j], p, 4);
    }
  } else if (a.type == 61) {
    unsigned stride = a.component_bits[0] + a.component_bits[1];
    uint64_t bit = uint64_t(index) * stride;
    for (unsigned j = 0; j < 2; ++j) {
      unsigned width = a.component_bits[j];
      uint32_t value = bits_at(a.packed_palette, bit, width);
      float extent = a.quantization_bounds[j + 2] - a.quantization_bounds[j];
      float step =
          width ? extent / static_cast<float>((1ull << width) - 1) : 0.f;
      out[j] = a.quantization_bounds[j] + static_cast<float>(value) * step;
      bit += width;
    }
  } else {
    require(a.bits == 8 || a.bits == 16, "unsupported attribute precision");
    for (unsigned j = 0; j < components; ++j) {
      uint32_t value =
          bits_at(a.packed_palette, (uint64_t(index) * components + j) * a.bits,
                  a.bits);
      if (normal) {
        int32_t signed_value = static_cast<int32_t>(value);
        if (value & (1u << (a.bits - 1)))
          signed_value -= (1u << a.bits);
        out[j] =
            a.type == 100
                ? (signed_value * static_cast<int32_t>(1u << (16 - a.bits))) /
                      32767.f
                : signed_value / static_cast<float>((1u << (a.bits - 1)) - 1);
      } else
        out[j] = value / static_cast<float>((1u << a.bits) - 1);
    }
  }
  return out;
}
} // namespace nwd
