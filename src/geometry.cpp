#include "internal.hpp"
namespace nwd::detail {
static std::vector<int32_t> indices(Cursor &r, uint32_t n) {
  require(n <= 100000000, "index count limit");
  std::vector<int32_t> out;
  out.reserve(n);
  int64_t highest = -1;
  auto append = [&](int32_t x, size_t count) {
    if (count > n - out.size())
      r.fail("index run exceeds declared count");
    out.insert(out.end(), count, x);
  };
  while (out.size() < n) {
    unsigned t = r.byte();
    if (t < 64) {
      require(t + 1 <= n - out.size(), "sequential run overrun");
      for (unsigned j = 0; j <= t; ++j)
        out.push_back(static_cast<int32_t>(++highest));
    } else if (t < 128) {
      unsigned d = t - 64;
      require(d && d <= out.size(), "invalid short index back-reference");
      out.push_back(out[out.size() - d]);
    } else if (t < 192) {
      require(!out.empty(), "repeat without previous index");
      append(out.back(), t - 127);
    } else if (t >= 0xd0 && t < 0xe0)
      out.push_back(-static_cast<int32_t>(t - 0xd0 + 1));
    else if (t >= 0xe0 && t < 0xf0) {
      unsigned d = (t - 0xe0) * 256 + r.byte();
      require(d && d <= out.size(), "invalid long index back-reference");
      out.push_back(out[out.size() - d]);
    } else if (t == 0xf0) {
      uint32_t v = r.byte();
      v = (v << 8) | r.byte();
      out.push_back(static_cast<int32_t>(v));
    } else if (t == 0xf1) {
      uint32_t v = 0;
      for (int j = 0; j < 4; ++j)
        v = (v << 8) | r.byte();
      require(v <= INT32_MAX, "literal index overflow");
      out.push_back(static_cast<int32_t>(v));
    } else
      r.fail("unsupported index opcode " + std::to_string(t));
  }
  r.align(4);
  return out;
}
static std::vector<uint32_t> unsigned_indices(Cursor &r, uint32_t n,
                                              uint32_t limit) {
  auto input = indices(r, n);
  std::vector<uint32_t> out;
  out.reserve(n);
  for (auto x : input) {
    require(x >= 0 && static_cast<uint32_t>(x) < limit,
            "geometry index outside palette");
    out.push_back(static_cast<uint32_t>(x));
  }
  return out;
}
static std::vector<uint32_t> packed(Cursor &r) {
  uint32_t bits = r.u32(), count = r.u32();
  require(bits <= 32 && (bits || !count), "invalid packed bit width");
  size_t nw = (static_cast<uint64_t>(bits) * count + 31) / 32;
  auto raw = r.raw(nw * 4);
  std::vector<uint32_t> words(nw);
  if (nw)
    std::memcpy(words.data(), raw.data(), raw.size());
  std::vector<uint32_t> out(count);
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t bit = i * bits;
    size_t w = bit / 32;
    unsigned shift = static_cast<unsigned>(bit % 32);
    uint64_t mask = bits == 32 ? UINT32_MAX : ((1ull << bits) - 1);
    uint64_t v = words[w];
    if (shift + bits <= 32)
      v >>= (32 - shift - bits);
    else {
      v = (v << 32) | words[w + 1];
      v >>= (64 - shift - bits);
    }
    out[i] = static_cast<uint32_t>(v & mask);
  }
  return out;
}
} // namespace nwd::detail
namespace nwd {
Geometry decode_geometry(std::span<const uint8_t> data, unsigned normal_bits,
                         bool raw_coordinates, bool raw_strips) {
  using namespace detail;
  require(normal_bits == 8 || normal_bits == 16,
          "unsupported normal precision");
  Cursor r(data, "geometry");
  require(r.u32() == 100, "geometry root ID must reset to 100");
  Geometry g;
  g.type = r.u32();
  if (g.type == 181) {
    auto e = std::make_shared<ExternalGeometry>();
    e->loader = r.string();
    e->format = r.string();
    e->source_path = r.string();
    e->payload_record_offset = r.pos;
    auto payload = r.raw(r.data.size() - r.pos);
    e->unparsed_payload.assign(payload.begin(), payload.end());
    g.external = std::move(e);
    return g;
  }
  if (g.type == 103) {
    g.text = r.string();
    for (unsigned j = 0; j < 12; ++j)
      g.parameters[j] = r.f32();
    g.text_style = r.u32();
    r.exact();
    return g;
  }
  if (g.type == 104 || g.type == 105) {
    g.flags = r.u32();
    unsigned nf = g.type == 104 ? 10 : 13;
    for (unsigned j = 0; j < nf; ++j)
      g.parameters[j] = r.f32();
    require(g.parameters[nf - 1] >= 0, "negative radius");
    r.exact();
    return g;
  }
  require(g.type == 94 || g.type == 95 || g.type == 96,
          "unsupported geometry type " + std::to_string(g.type));
  require(r.u32() == 101 && r.u32() == 60, "unsupported coordinate object");
  uint32_t n = r.u32();
  require(n <= 100000000, "coordinate count limit");
  uint32_t nf = raw_coordinates ? n * 3 : r.u32();
  require(nf % 3 == 0 && nf <= (data.size() - r.pos) / 4,
          "invalid coordinate palette");
  g.coordinates.resize(nf);
  for (auto &x : g.coordinates)
    x = r.f32();
  if (nf == uint64_t(n) * 3) {
    g.coordinate_indices.resize(n);
    for (uint32_t i = 0; i < n; ++i)
      g.coordinate_indices[i] = i;
  } else
    g.coordinate_indices = unsigned_indices(r, n, nf / 3);
  for (unsigned slot = 0; slot < 3; ++slot) {
    uint32_t oid = r.u32();
    if (!oid)
      continue;
    require(oid >= 102, "unexpected attribute object reference");
    AttributeArray a;
    a.type = r.u32();
    if (a.type == 58) {
      a.flag = r.u32();
      a.bits = 8;
    } else if (a.type == 100 || a.type == 59)
      a.bits = normal_bits;
    else if (a.type == 61)
      a.bits = 32;
    else
      r.fail("unsupported vertex attribute " + std::to_string(a.type));
    int32_t slots = r.read<int32_t>();
    if (a.type == 61 && slots < 0) {
      require(slots == -static_cast<int64_t>(n), "UV slot count mismatch");
      for (auto &x : a.quantization_bounds) {
        x = r.read<float>();
        a.finite = a.finite && std::isfinite(x);
      }
      for (auto &x : a.component_bits) {
        x = r.byte();
        require(x <= 32, "UV component bit width");
      }
      a.palette_count = r.u32();
      auto raw = r.raw(4 * ((uint64_t(a.palette_count) *
                                 (a.component_bits[0] + a.component_bits[1]) +
                             31) /
                            32));
      a.packed_palette.assign(raw.begin(), raw.end());
      if (a.palette_count < n)
        a.indices = indices(r, n);
      else {
        a.indices.resize(n);
        for (uint32_t j = 0; j < n; ++j)
          a.indices[j] = j;
      }
      for (auto index : a.indices)
        require(index >= 0 && uint32_t(index) < a.palette_count,
                "UV index outside palette");
      g.attributes.emplace_back(std::move(a));
      continue;
    }
    if (a.type == 61 && slots >= 0) {
      require(static_cast<uint32_t>(slots) == n, "UV slot count mismatch");
      auto floats = r.u32();
      require(floats % 2 == 0, "UV float palette size");
      a.palette_count = floats / 2;
      a.raw = true;
      a.bits = 32;
      auto raw = r.raw(uint64_t(floats) * 4);
      a.packed_palette.assign(raw.begin(), raw.end());
      for (uint32_t j = 0; j < floats; ++j) {
        float v;
        std::memcpy(&v, raw.data() + 4ull * j, 4);
        a.finite = a.finite && std::isfinite(v);
      }
      if (a.palette_count < n)
        a.indices = indices(r, n);
      else {
        a.indices.resize(n);
        for (uint32_t j = 0; j < n; ++j)
          a.indices[j] = j;
      }
      for (auto index : a.indices)
        require(index >= 0 && uint32_t(index) < a.palette_count,
                "UV float index outside palette");
      g.attributes.emplace_back(std::move(a));
      continue;
    }
    if (slots >= 0) {
      require(static_cast<uint32_t>(slots) == n, "raw attribute slot mismatch");
      a.palette_count = slots;
      a.raw = true;
      a.bits = a.type == 100 ? 16 : a.type == 58 ? 8 : 32;
      unsigned components = a.type == 61 ? 2 : a.type == 58 ? 4 : 3;
      auto raw = r.raw(uint64_t(n) * components * a.bits / 8);
      a.packed_palette.assign(raw.begin(), raw.end());
      a.indices.resize(n);
      for (uint32_t j = 0; j < n; ++j)
        a.indices[j] = j;
      g.attributes.emplace_back(std::move(a));
      continue;
    }
    a.palette_count = r.u32();
    require(slots == -static_cast<int64_t>(n), "attribute slot count mismatch");
    unsigned components = a.type == 58 ? 4 : 3;
    size_t bytes =
        4 * ((uint64_t(a.palette_count) * components * a.bits + 31) / 32);
    auto raw = r.raw(bytes);
    a.packed_palette.assign(raw.begin(), raw.end());
    if (a.palette_count >= n) {
      a.indices.resize(n);
      for (uint32_t j = 0; j < n; ++j)
        a.indices[j] = static_cast<int32_t>(j);
    } else
      a.indices = indices(r, n);
    for (auto j : a.indices)
      require(j >= (a.type == 100 || a.type == 59 ? -6 : 0) &&
                  (j < 0 || static_cast<uint32_t>(j) < a.palette_count),
              "attribute index outside palette");
    g.attributes.emplace_back(std::move(a));
  }
  if (g.type == 96) {
    g.flags = r.u32();
    require(g.flags <= 1, "point-set boolean field");
    r.exact();
    return g;
  }
  uint32_t groups = r.u32();
  if (raw_strips) {
    auto bytes = r.raw(uint64_t(groups) * 2);
    g.strip_lengths.resize(groups);
    for (uint32_t i = 0; i < groups; ++i) {
      uint16_t x;
      std::memcpy(&x, bytes.data() + i * 2, 2);
      g.strip_lengths[i] = x;
    }
  } else
    g.strip_lengths = packed(r);
  require(g.strip_lengths.size() == groups, "strip group count mismatch");
  uint32_t ns = g.type == 94 ? r.u32() : 0;
  if (ns && raw_strips) {
    auto bytes = r.raw(uint64_t(ns) * 2);
    g.strip_indices.resize(ns);
    for (uint32_t i = 0; i < ns; ++i) {
      uint16_t x;
      std::memcpy(&x, bytes.data() + i * 2, 2);
      require(x < n, "raw strip index outside palette");
      g.strip_indices[i] = x;
    }
  } else if (ns)
    g.strip_indices = unsigned_indices(r, ns, n);
  else {
    g.strip_indices.resize(n);
    for (uint32_t j = 0; j < n; ++j)
      g.strip_indices[j] = j;
  }
  uint64_t total = 0;
  for (auto len : g.strip_lengths)
    total += len;
  require(total == g.strip_indices.size(), "strip length/index count mismatch");
  r.exact();
  return g;
}
std::vector<uint32_t> triangle_indices(const Geometry &g) {
  std::vector<uint32_t> out;
  if (g.type != 94)
    return out;
  uint64_t total = 0;
  for (auto n : g.strip_lengths)
    total += n;
  detail::require(total == g.strip_indices.size(),
                  "strip lengths do not cover indices");
  for (auto index : g.strip_indices)
    detail::require(index < g.coordinate_indices.size() &&
                        g.coordinate_indices[index] < g.coordinates.size() / 3,
                    "triangle vertex outside coordinate palette");
  size_t p = 0;
  for (auto n : g.strip_lengths) {
    for (uint32_t j = 0; j + 2 < n; ++j) {
      uint32_t a = g.strip_indices[p + j], b = g.strip_indices[p + j + 1],
               c = g.strip_indices[p + j + 2];
      if (j % 2)
        std::swap(a, b);
      if (g.coordinate_indices[a] != g.coordinate_indices[b] &&
          g.coordinate_indices[b] != g.coordinate_indices[c] &&
          g.coordinate_indices[c] != g.coordinate_indices[a])
        out.insert(out.end(), {a, b, c});
    }
    p += n;
  }
  return out;
}
} // namespace nwd
namespace nwd::detail {
void read_geometry(Model &model, std::span<const uint8_t> file, const Chunk &c,
                   const Options &o) {
  require(c.flags == 1, "unsupported geometry chunk encoding");
  require(c.prefix_bytes && c.index_bytes &&
              c.index_bytes < c.size - c.prefix_bytes,
          "geometry requires block index");
  auto index = inflate_one(
      file.subspan(c.offset + c.size - c.index_bytes, c.index_bytes),
      o.max_decoded_chunk);
  require(index.consumed == c.index_bytes,
          "geometry index stream length mismatch");
  Cursor r(index.bytes, "geometry block index");
  uint32_t count = r.u32(), nb = r.u32();
  require(count <= o.max_objects && nb <= 10000000,
          "geometry index resource limit");
  require(uint64_t(count) + nb <= (r.data.size() - r.pos) / 4,
          "geometry index table truncated");
  std::vector<uint32_t> block_sizes(nb), record_sizes(count);
  uint64_t compressed = 0, decoded = 0;
  for (auto &n : block_sizes) {
    n = r.u32();
    compressed += n;
  }
  for (auto &n : record_sizes) {
    n = r.u32();
    decoded += n;
  }
  r.exact();
  require(compressed + c.prefix_bytes + c.index_bytes == c.size,
          "geometry block table size mismatch");
  require(decoded <= o.max_decoded_chunk, "geometry output resource limit");
  std::vector<uint64_t> offsets(nb + 1);
  offsets[0] = c.offset + c.prefix_bytes;
  for (size_t i = 0; i < nb; ++i)
    offsets[i + 1] = offsets[i] + block_sizes[i];
  std::vector<Bytes> blocks(nb);
  parallel_for(nb, o.threads, [&](size_t i) {
    auto x = inflate_one(file.subspan(offsets[i], block_sizes[i]),
                         o.max_decoded_chunk);
    require(x.consumed == block_sizes[i], "geometry block compressed boundary");
    blocks[i] = std::move(x.bytes);
  });
  Bytes bytes;
  bytes.reserve(static_cast<size_t>(decoded));
  for (auto &b : blocks) {
    bytes.insert(bytes.end(), b.begin(), b.end());
    Bytes().swap(b);
  }
  require(bytes.size() == decoded, "geometry decoded size mismatch");
  std::vector<size_t> record_offsets(count + 1);
  for (size_t i = 0; i < count; ++i)
    record_offsets[i + 1] = record_offsets[i] + record_sizes[i];
  model.geometries.resize(count);
  auto parse = [&](unsigned bits, bool raw = false) {
    parallel_for(count, o.threads, [&](size_t i) {
      try {
        model.geometries[i] = decode_geometry(
            std::span(bytes).subspan(record_offsets[i], record_sizes[i]), bits,
            raw, raw);
      } catch (const Error &e) {
        throw Error("record " + std::to_string(i + 1) + ": " + e.what());
      }
    });
    model.normal_bits = bits;
    model.raw_coordinates = raw;
  };
  if (o.normal_bits)
    parse(o.normal_bits);
  else {
    try {
      parse(8);
    } catch (const Error &first) {
      std::string reason = first.what();
      try {
        parse(16);
      } catch (const Error &second) {
        std::string reason2 = second.what();
        try {
          parse(8, true);
        } catch (const Error &third) {
          throw Error("8-bit: " + reason + "; 16-bit: " + reason2 +
                      "; raw coordinates: " + third.what());
        }
      }
    }
  }
}
} // namespace nwd::detail
