#include "internal.hpp"
#include "blowfish.hpp"
#define CBC 0
#define CTR 0
#define AES_init_ctx nwd_lwa_aes_init
#define AES_ECB_decrypt nwd_lwa_aes_decrypt
#define AES_ECB_encrypt nwd_lwa_aes_encrypt
extern "C" {
#include "tiny-aes/aes.h"
}
namespace nwd::detail {
namespace {
struct Budget {
  uint64_t objects, bytes;
  explicit Budget(const Options &o)
      : objects(o.max_objects), bytes(o.max_decoded_chunk) {}
  void items(uint64_t n = 1) {
    require(n <= objects, "LightWorks object/value resource limit");
    objects -= n;
  }
  void decoded(uint64_t n) {
    require(n <= bytes, "LightWorks decoded byte resource limit");
    bytes -= n;
  }
};
struct BE : Cursor {
  using Cursor::Cursor;
  uint32_t u() {
    auto s = raw(4);
    return uint32_t(s[0]) << 24 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 8 |
           s[3];
  }
  uint64_t wide() {
    auto high = u();
    return uint64_t(high) << 32 | u();
  }
  std::string text() {
    auto s = raw(u());
    return {reinterpret_cast<const char *>(s.data()), s.size()};
  }
};
// Seed-zero ISAAC32. The archive key registry consumes the first batch in
// ascending order. Arithmetic follows Bob Jenkins' public-domain algorithm.
std::array<std::array<uint8_t, 16>, 64> default_keys() {
  std::array<uint32_t, 256> m{}, result{};
  std::array<uint32_t, 8> v;
  v.fill(0x9e3779b9);
  constexpr int shifts[]{11, -2, 8, -16, 10, -4, 8, -9};
  auto mix = [&] {
    for (unsigned i = 0; i < 8; ++i) {
      auto j = (i + 1) % 8;
      v[i] ^= shifts[i] > 0 ? v[j] << shifts[i] : v[j] >> -shifts[i];
      v[(i + 3) % 8] += v[i];
      v[j] += v[(i + 2) % 8];
    }
  };
  for (unsigned i = 0; i < 4; ++i)
    mix();
  for (unsigned pass = 0; pass < 2; ++pass)
    for (unsigned i = 0; i < 256; i += 8) {
      if (pass)
        for (unsigned j = 0; j < 8; ++j)
          v[j] += m[i + j];
      mix();
      std::copy(v.begin(), v.end(), m.begin() + i);
    }
  uint32_t a = 0, b = 1;
  constexpr int rounds[]{13, -6, 2, -16};
  for (unsigned i = 0; i < 256; ++i) {
    auto x = m[i];
    int shift = rounds[i % 4];
    a ^= shift > 0 ? a << shift : a >> -shift;
    a += m[(i + 128) % 256];
    auto y = m[(x >> 2) & 255] + a + b;
    m[i] = y;
    result[i] = b = m[(y >> 10) & 255] + x;
  }
  std::array<std::array<uint8_t, 16>, 64> keys;
  for (unsigned i = 0; i < 64; ++i)
    std::memcpy(keys[i].data(), result.data() + i * 4, 16);
  return keys;
}
Bytes base64(std::string_view s) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  Bytes out;
  unsigned bits = 0, value = 0, padding = 0, chars = 0;
  for (char c : s) {
    if (c == '\r' || c == '\n')
      continue;
    ++chars;
    if (c == '=') {
      ++padding;
      continue;
    }
    auto n = alphabet.find(c);
    require(!padding && n != alphabet.npos, "LightWorks key base64");
    value = (value << 6) | unsigned(n);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t(value >> bits));
    }
  }
  require(chars % 4 == 0 && padding <= 2 && bits == padding * 2 &&
              (value & ((1u << bits) - 1)) == 0,
          "LightWorks key base64 padding");
  return out;
}
void decrypt(Bytes &data, std::span<const uint8_t> key, uint32_t algorithm) {
  if (algorithm == 4) {
    require(data.size() % 8 == 0, "LightWorks Blowfish block size");
    Blowfish cipher(key);
    for (size_t i = 0; i < data.size(); i += 8) {
      uint32_t l, r;
      std::memcpy(&l, data.data() + i, 4);
      std::memcpy(&r, data.data() + i + 4, 4);
      cipher.decrypt(l, r);
      std::memcpy(data.data() + i, &l, 4);
      std::memcpy(data.data() + i + 4, &r, 4);
    }
  } else if (algorithm == 1) {
    require(key.size() == 16 && data.size() % 16 == 0,
            "LightWorks AES block/key size");
    AES_ctx context;
    AES_init_ctx(&context, key.data());
    for (size_t i = 0; i < data.size(); i += 16)
      AES_ECB_decrypt(&context, data.data() + i);
  } else
    throw UnsupportedLayout("LightWorks cipher " + std::to_string(algorithm));
}
LightWorksValue value(BE &r, uint32_t type, Budget &budget) {
  switch (type) {
  case 1:
  case 2:
  case 5:
  case 39:
    return r.u();
  case 6:
  case 45:
    return uint32_t(r.byte());
  case 3:
    return double(std::bit_cast<float>(r.u()));
  case 4:
    return std::bit_cast<double>(r.wide());
  case 10:
    return r.text();
  case 32: {
    auto n = r.u();
    budget.items(n);
    require(n <= (r.data.size() - r.pos) / 4, "LightWorks string array");
    std::vector<std::string> out;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
      out.push_back(r.text());
    return out;
  }
  case 47: {
    auto s = r.raw(r.u());
    return Bytes(s.begin(), s.end());
  }
  case 7:
  case 8:
  case 9:
  case 31:
  case 42:
  case 43:
  case 44: {
    unsigned n = type == 31 ? 4 : type == 44 ? 6 : type >= 42 ? 2 : 3;
    budget.items(n);
    std::vector<double> out;
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i)
      out.push_back(double(std::bit_cast<float>(r.u())));
    return out;
  }
  case 23:
  case 24:
  case 27:
  case 41: {
    auto n = r.u();
    budget.items(n);
    require(n <= (r.data.size() - r.pos) / 4, "LightWorks integer array");
    std::vector<uint32_t> out;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
      out.push_back(r.u());
    return out;
  }
  case 25:
  case 26: {
    auto n = r.u();
    budget.items(n);
    require(n <= (r.data.size() - r.pos) / (type == 25 ? 4 : 8),
            "LightWorks real array");
    std::vector<double> out;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
      out.push_back(type == 25 ? double(std::bit_cast<float>(r.u()))
                               : std::bit_cast<double>(r.wide()));
    return out;
  }
  default:
    throw UnsupportedLayout("LightWorks value type " + std::to_string(type));
  }
}
#include "lightworks_schema.inc"
void objects(LightWorksArchive &out, BE &r, Budget &budget) {
  std::unordered_map<uint32_t, size_t> definitions;
  for (size_t i = 0; i < out.definitions.size(); ++i)
    require(definitions.emplace(out.definitions[i].type, i).second,
            "LightWorks duplicate type definition");
  std::unordered_map<uint32_t, Id> identities;
  std::vector<Id> stack, last_children;
  while (r.pos < r.data.size()) {
    auto raw = r.u();
    if (raw == none) {
      require(!stack.empty(), "LightWorks unmatched object end");
      stack.pop_back();
      last_children.pop_back();
      continue;
    }
    if (raw & 0xfc000000)
      throw UnsupportedLayout("LightWorks object header flags");
    budget.items();
    require(out.objects.size() < none, "LightWorks object ID overflow");
    auto index = static_cast<Id>(out.objects.size());
    auto &o = out.objects.emplace_back();
    o.type = raw & 0xffffff;
    o.header_flags = raw >> 24;
    o.flags = r.byte();
    if (raw & 0x2000000) {
      auto bytes = r.raw(4);
      std::copy(bytes.begin(), bytes.end(), o.extension.emplace().begin());
    }
    if (raw & 0x1000000)
      o.identity = r.u();
    if (!stack.empty()) {
      o.parent = stack.back();
      if (last_children.back() == none)
        out.objects[o.parent].first_child = index;
      else
        out.objects[last_children.back()].next_sibling = index;
      last_children.back() = index;
    }
    if (o.identity) {
      require(*o.identity != 0 && *o.identity != none,
              "LightWorks invalid object identity");
      if (*o.identity == 1) {
        o.null_reference = true;
        continue;
      }
      auto [it, fresh] = identities.emplace(*o.identity, index);
      if (!fresh) {
        require(out.objects[it->second].type == o.type,
                "LightWorks ambiguous reference class");
        o.reference = it->second;
        continue;
      }
    }
    auto it = definitions.find(o.type);
    if (it == definitions.end())
      throw UnsupportedLayout("LightWorks object class " +
                              std::to_string(o.type));
    const auto &definition = out.definitions[it->second];
    for (;;) {
      auto id = out.stream_flags & 4 ? uint32_t(r.byte()) : r.u();
      if (id == (out.stream_flags & 4 ? 255u : none))
        break;
      budget.items();
      auto field =
          std::find_if(definition.fields.begin(), definition.fields.end(),
                       [&](const auto &f) { return f.id == id; });
      require(field != definition.fields.end(), "LightWorks undefined field");
      auto type = field->type;
      if (!type)
        type = out.stream_flags & 2 ? uint32_t(r.byte()) : r.u();
      o.fields.push_back({id, type, field->name, value(r, type, budget)});
    }
    require(stack.size() < 128, "LightWorks object depth limit");
    stack.push_back(index);
    last_children.push_back(none);
  }
  require(stack.empty(), "LightWorks unterminated object");
}
LightWorksArchive archive(Cursor &source, Budget &budget) {
  BE r(source.data.subspan(source.pos), "LightWorks archive");
  auto magic = r.raw(5);
  require(std::memcmp(magic.data(), "LiLWA", 5) == 0,
          "LightWorks archive magic");
  auto header = r.raw(r.u());
  BE h(header, "LightWorks header");
  LightWorksArchive out;
  out.engine_version = h.u();
  if (!out.engine_version) {
    out.engine_version = h.u();
    out.encoding = h.u();
  }
  out.block_bits = h.byte();
  out.flags = h.byte();
  if ((out.flags & ~3u) || out.encoding > 1)
    throw UnsupportedLayout("LightWorks archive encoding/envelope");
  require(out.block_bits >= 7 && out.block_bits <= 24,
          "LightWorks block size exponent");
  const bool compressed = (out.flags & 2) != 0;
  const bool encrypted = (out.flags & 1) != 0;
  if (compressed)
    out.compression = h.u();
  if (encrypted) {
    auto parameters = h.u();
    out.cipher = h.u();
    if (out.cipher != 1 && out.cipher != 4)
      throw UnsupportedLayout("LightWorks archive cipher");
    if (parameters) {
      if (out.encoding == 1)
        out.key_name = h.text();
      else {
        // Both supported native cipher classes report a 16-byte key.
        require(parameters >= 16, "LightWorks legacy key parameter length");
        auto bytes = h.raw(16);
        out.encoded_key.assign(bytes.begin(), bytes.end());
        parameters -= 16;
      }
      auto p = h.raw(parameters);
      out.cipher_parameters.assign(p.begin(), p.end());
    }
  }
  out.header_value = h.u();
  require(h.pos == h.data.size(), "LightWorks header tail");
  if (compressed && out.compression != 1)
    throw UnsupportedLayout("LightWorks archive compression");
  BE meta(r.raw(r.u()), "LightWorks definitions");
  out.stream_flags = meta.u();
  out.root_page = meta.u();
  if (out.stream_flags & ~0x3fu)
    throw UnsupportedLayout("LightWorks stream flags");
  for (;;) {
    auto type = meta.u();
    if (type == none)
      break;
    budget.items();
    auto &d = out.definitions.emplace_back();
    d.type = type;
    d.version = meta.u();
    d.stored = true;
    if (out.stream_flags & 16)
      d.name = meta.text();
    for (;;) {
      auto id = out.stream_flags & 4 ? uint32_t(meta.byte()) : meta.u();
      if (id == (out.stream_flags & 4 ? 255u : none))
        break;
      budget.items();
      auto &f = d.fields.emplace_back();
      f.id = id;
      if ((out.stream_flags & 16) && id)
        f.name = meta.text();
      f.type = out.stream_flags & 2 ? uint32_t(meta.byte()) : meta.u();
      require(std::count_if(d.fields.begin(), d.fields.end(),
                            [&](const auto &x) { return x.id == id; }) == 1,
              "LightWorks duplicate field definition");
    }
  }
  require(meta.pos == meta.data.size(), "LightWorks definition table tail");
  add_known_definitions(out);
  Bytes key;
  if (encrypted) {
    if (out.encoding == 0) {
      require(out.encoded_key.size() == 16, "LightWorks missing legacy key");
      uint8_t cumulative = 0;
      for (auto it = out.encoded_key.rbegin(); it != out.encoded_key.rend();
           ++it) {
        cumulative ^= *it;
        key.push_back(cumulative);
      }
    } else {
      auto wrapped = base64(out.key_name);
      require(wrapped.size() == 17 && wrapped.front() < 64,
              "LightWorks archive key identifier");
      static const auto keys = default_keys();
      auto key_index = wrapped.front();
      key.assign(wrapped.begin() + 1, wrapped.end());
      // Wrapping and payload algorithms are selected independently.
      decrypt(key, keys[key_index], key_index < 32 ? 4 : 1);
    }
  }
  auto size = uint64_t(1) << out.block_bits;
  struct Page {
    Id next;
    uint32_t valid;
    std::span<const uint8_t> raw;
    bool visited = false;
  };
  auto read_page = [&]() {
    budget.items();
    require(out.frame_count < none, "LightWorks frame count overflow");
    ++out.frame_count;
    uint64_t raw_size = size;
    if (compressed) {
      auto frame_size = r.u();
      require(frame_size >= 12, "LightWorks frame size");
      raw_size = frame_size - 12;
    }
    auto next = r.u();
    auto valid = r.u();
    require(valid <= size, "LightWorks frame valid byte count");
    return Page{next, valid, r.raw(static_cast<size_t>(raw_size))};
  };
  auto decode_page = [&](const Page &page, Bytes &decoded) {
    budget.decoded(size);
    auto raw = page.raw;
    if (encrypted) {
      const size_t block = out.cipher == 4 ? 8 : 16;
      require(!raw.empty() &&
                  (raw.size() % block == 0 || raw.size() % block == 1),
              "LightWorks cipher frame size");
      decoded.assign(raw.begin(), raw.begin() + raw.size() / block * block);
      decrypt(decoded, key, out.cipher);
      if (raw.size() % block) {
        require(raw.back() < block && raw.back() <= decoded.size(),
                "LightWorks cipher padding length");
        decoded.resize(decoded.size() - raw.back());
      }
      raw = decoded;
    }
    if (compressed) {
      auto plain = inflate_one(raw, size, static_cast<size_t>(size));
      require(plain.consumed == raw.size() && plain.bytes.size() == size,
              "LightWorks compressed frame length");
      decoded = std::move(plain.bytes);
      raw = decoded;
    }
    require(raw.size() == size, "LightWorks decoded frame length");
    return raw.first(page.valid);
  };
  auto first = read_page();
  if (out.root_page == 0 && first.next == none) {
    // Usual single-page archives need no page index or payload concatenation.
    Bytes decoded;
    BE payload(decode_page(first, decoded), "LightWorks objects");
    objects(out, payload, budget);
    out.page_order.push_back(0);
  } else {
    // The native random-access index is built from physical frame lengths,
    // not serialized as a separate directory. Build it only as far as needed.
    std::vector<Page> pages{first};
    Bytes stream, decoded;
    Id id = out.root_page;
    require(id != none, "LightWorks missing root page");
    while (id != none) {
      require(uint64_t(id) < uint64_t(pages.size()) + budget.objects,
              "LightWorks page index resource limit");
      while (pages.size() <= id)
        pages.push_back(read_page());
      auto &page = pages[id];
      require(!page.visited, "LightWorks cyclic page chain");
      page.visited = true;
      out.page_order.push_back(id);
      auto bytes = decode_page(page, decoded);
      stream.insert(stream.end(), bytes.begin(), bytes.end());
      id = page.next;
    }
    if (std::any_of(pages.begin(), pages.end(),
                    [](const Page &p) { return !p.visited; }))
      throw UnsupportedLayout("LightWorks physical pages outside root stream");
    BE payload(stream, "LightWorks objects");
    objects(out, payload, budget);
  }
  source.pos += r.pos;
  return out;
}
uint32_t count(Cursor &r, Budget &b) {
  auto n = r.u32();
  b.items(n);
  require(n <= (r.data.size() - r.pos) / 4, "Presenter array size");
  return n;
}
bool boolean(Cursor &r) {
  auto x = r.u32();
  require(x <= 1, "Presenter boolean");
  return x != 0;
}
LegacyShader shader(Cursor &r, Budget &budget) {
  LegacyShader out;
  out.name = r.string();
  for (auto n = count(r, budget); n; --n) {
    auto &a = out.arguments.emplace_back();
    a.name = r.string();
    a.type = r.u32();
    switch (a.type) {
    case 1:
    case 2:
    case 5:
    case 6:
      a.value = r.u32();
      break;
    case 3:
      a.value = r.read<double>();
      break;
    case 7:
    case 8:
    case 9: {
      std::vector<double> v;
      for (unsigned i = 0; i < 3; ++i)
        v.push_back(r.read<double>());
      a.value = std::move(v);
      break;
    }
    case 10:
      a.value = r.string();
      break;
    default:
      throw UnsupportedLayout("Presenter legacy shader argument type");
    }
  }
  return out;
}
PresenterEntity entity(Cursor &r, std::vector<LightWorksArchive> &archives,
                       Budget &budget, bool material = false) {
  budget.items();
  PresenterEntity out;
  out.mode = r.u32();
  if (out.mode == 2)
    return out;
  if (boolean(r)) {
    require(archives.size() < none, "Presenter archive ID overflow");
    auto a = archive(r, budget);
    out.archive = static_cast<Id>(archives.size());
    archives.push_back(std::move(a));
  } else if (material) {
    // WideString uses the common UTF-8 stream string encoding.
    out.legacy_material_name = r.string();
    for (auto &s : out.legacy_material_shaders)
      if (boolean(r))
        s = shader(r, budget);
  } else
    out.legacy_shader = shader(r, budget);
  return out;
}
std::vector<uint32_t> selector(Cursor &r, bool implicit, bool node, Budget &b) {
  return read_path_selector(r, implicit, node, b.objects);
}
} // namespace
void read_presenter(PresenterData &out, Cursor &r, uint32_t version,
                    bool implicit, const Options &options) {
  if (version < 82)
    throw UnsupportedLayout("Presenter version below 82");
  Budget budget(options);
  out.implicit_node_map = implicit;
  if (version >= 101)
    out.enabled = boolean(r);
  out.background = entity(r, out.archives, budget);
  for (auto n = count(r, budget); n; --n)
    out.materials.push_back(entity(r, out.archives, budget, true));
  for (unsigned scope = 0; scope < 2; ++scope)
    for (auto n = count(r, budget); n; --n) {
      auto &binding = out.material_bindings.emplace_back();
      binding.node_scope = scope != 0;
      binding.paths = selector(r, implicit, scope != 0, budget);
      binding.material = r.u32();
      require(binding.material == none ||
                  binding.material < out.materials.size(),
              "Presenter material slot out of range");
    }
  for (unsigned scope = 0; scope < 2; ++scope)
    for (auto n = count(r, budget); n; --n) {
      auto &binding = out.texture_bindings.emplace_back();
      binding.node_scope = scope != 0;
      binding.paths = selector(r, implicit, scope != 0, budget);
      binding.mapping = r.u32();
      if (binding.mapping != 0 && binding.mapping != 4 && binding.mapping != 5)
        binding.entity = entity(r, out.archives, budget);
    }
}
void read_presenter_lights(PresenterLights &out, Cursor &r, uint32_t version,
                           const Options &options) {
  if (version < 82)
    throw UnsupportedLayout("Presenter lights version below 82");
  Budget budget(options);
  for (auto n = count(r, budget); n; --n)
    out.lights.push_back(entity(r, out.archives, budget));
}
} // namespace nwd::detail
namespace nwd {
LightWorksImage decode_lightworks_image(const LightWorksObject &object,
                                        uint64_t max_bytes) {
  using namespace detail;
  require(object.type == 0x12 && object.reference == none &&
              !object.null_reference,
          "LightWorks image requires a resolved LtImage object");
  auto field = [&](uint32_t id, uint32_t type) -> const LightWorksValue & {
    const LightWorksField *found = nullptr;
    for (const auto &f : object.fields)
      if (f.id == id) {
        require(!found && f.type == type,
                "LightWorks image field type/duplicate");
        found = &f;
      }
    require(found != nullptr, "LightWorks image field missing");
    return found->value;
  };
  auto integer = [&](uint32_t id, uint32_t type = 2) {
    auto p = std::get_if<uint32_t>(&field(id, type));
    require(p != nullptr, "LightWorks image integer storage");
    return *p;
  };
  LightWorksImage image;
  image.width = integer(1);
  image.height = integer(2);
  image.bits_per_pixel = integer(3);
  auto codec = integer(11, 5);
  auto pixels = uint64_t(image.width) * image.height;
  auto bytes_per_pixel = (uint64_t(image.bits_per_pixel) + 7) / 8;
  require(pixels && bytes_per_pixel && pixels <= max_bytes / bytes_per_pixel,
          "LightWorks image decoded size/resource limit");
  auto size = pixels * bytes_per_pixel;
  require(size <= std::numeric_limits<size_t>::max(),
          "LightWorks image exceeds addressable memory");
  auto raw = std::get_if<Bytes>(&field(0, 47));
  require(raw != nullptr, "LightWorks image byte storage");
  if (codec == 1) {
    require(raw->size() == size, "LightWorks raw image length");
    image.pixels = *raw;
  } else if (codec == 2) {
    auto decoded = inflate_one(*raw, size, static_cast<size_t>(size));
    require(decoded.consumed == raw->size() && decoded.bytes.size() == size,
            "LightWorks image zlib length");
    image.pixels = std::move(decoded.bytes);
  } else
    throw UnsupportedLayout("LightWorks image codec " + std::to_string(codec));
  return image;
}
} // namespace nwd
