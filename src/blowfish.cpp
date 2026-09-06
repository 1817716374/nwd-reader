#include "internal.hpp"
#include "blowfish.hpp"
#include "blowfish_constants.hpp"
#include <zlib.h>
namespace nwd::detail {
std::vector<uint8_t> decode_chunk_cipher(std::span<const uint8_t> bytes,
                                         uint64_t limit) {
  static constexpr uint8_t key[] = {
      0x20, 0xd9, 0xaf, 0xe0, 0x80, 0xf1, 0x17, 0x52, 0xde, 0x9d, 0xd9,
      0x12, 0x43, 0xde, 0x1a, 0x98, 0x58, 0x1e, 0x2d, 0xb0, 0x4d, 0x78,
      0x2f, 0xe4, 0x69, 0x21, 0x8e, 0x52, 0xd7, 0x05, 0x1f, 0x07};
  static const Blowfish cipher(key);
  require(!bytes.empty() && bytes.size() % 8 == 0,
          "chunk cipher block alignment");
  Bytes out;
  size_t pos = 0;
  auto pair = [&]() {
    require(bytes.size() - pos >= 8, "truncated cipher block");
    uint32_t a, b;
    std::memcpy(&a, bytes.data() + pos, 4);
    std::memcpy(&b, bytes.data() + pos + 4, 4);
    pos += 8;
    cipher.decrypt(a, b);
    return std::pair{a, b};
  };
  while (pos < bytes.size()) {
    auto [length, checksum] = pair();
    require(length > 0 && length <= INT32_MAX && length <= limit - out.size(),
            "chunk cipher segment length/resource limit");
    uint64_t padded = (uint64_t(length) + 7) & ~uint64_t(7);
    require(padded <= bytes.size() - pos, "truncated cipher segment");
    size_t begin = out.size();
    out.resize(begin + length);
    for (uint64_t j = 0; j < padded; j += 8) {
      auto [a, b] = pair();
      uint8_t decoded[8];
      std::memcpy(decoded, &a, 4);
      std::memcpy(decoded + 4, &b, 4);
      std::memcpy(out.data() + begin + j, decoded,
                  std::min<uint64_t>(8, length - j));
    }
    require(adler32(1, out.data() + begin, length) == checksum,
            "chunk cipher segment checksum");
  }
  return out;
}
uint32_t Blowfish::f(uint32_t x) const {
  return ((s_[0][x >> 24] + s_[1][(x >> 16) & 255]) ^ s_[2][(x >> 8) & 255]) +
         s_[3][x & 255];
}
void Blowfish::encrypt(uint32_t &l, uint32_t &r) const {
  for (size_t i = 0; i < 16; ++i) {
    l ^= p_[i];
    r ^= f(l);
    std::swap(l, r);
  }
  std::swap(l, r);
  r ^= p_[16];
  l ^= p_[17];
}
void Blowfish::decrypt(uint32_t &l, uint32_t &r) const {
  for (size_t i = 17; i > 1; --i) {
    l ^= p_[i];
    r ^= f(l);
    std::swap(l, r);
  }
  std::swap(l, r);
  r ^= p_[1];
  l ^= p_[0];
}
Blowfish::Blowfish(std::span<const uint8_t> key) {
  require(!key.empty() && key.size() <= 56, "Blowfish key length");
  using namespace blowfish_constants;
  std::copy_n(p, 18, p_.begin());
  std::copy_n(s0, 256, s_[0].begin());
  std::copy_n(s1, 256, s_[1].begin());
  std::copy_n(s2, 256, s_[2].begin());
  std::copy_n(s3, 256, s_[3].begin());
  size_t k = 0;
  for (auto &x : p_) {
    uint32_t word = 0;
    for (unsigned i = 0; i < 4; ++i) {
      word = (word << 8) | key[k];
      k = (k + 1) % key.size();
    }
    x ^= word;
  }
  uint32_t l = 0, r = 0;
  auto expand = [&](auto &a) {
    for (size_t i = 0; i < a.size(); i += 2) {
      encrypt(l, r);
      a[i] = l;
      a[i + 1] = r;
    }
  };
  expand(p_);
  for (auto &a : s_)
    expand(a);
}
std::string decode_database_connection(std::span<const uint8_t> bytes) {
  if (bytes.empty())
    return {};
  require(bytes.size() % 8 == 0, "database connection block length");
  // Format constant; no user credentials or installation-specific state.
  static constexpr std::string_view key = "g1eki+mIaqIEStIe8lap";
  static const Blowfish cipher(
      {reinterpret_cast<const uint8_t *>(key.data()), key.size()});
  std::string out;
  out.reserve(bytes.size());
  uint32_t high = 0;
  auto append = [&](uint16_t unit) {
    uint32_t c = unit;
    if (high) {
      require(c >= 0xdc00 && c <= 0xdfff,
              "database connection UTF-16 surrogate");
      c = 0x10000 + ((high - 0xd800) << 10) + (c - 0xdc00);
      high = 0;
    } else if (c >= 0xd800 && c <= 0xdbff) {
      high = c;
      return;
    } else
      require(c < 0xdc00 || c > 0xdfff, "database connection UTF-16 surrogate");
    if (c < 0x80)
      out.push_back(static_cast<char>(c));
    else {
      if (c < 0x800)
        out.push_back(static_cast<char>(0xc0 | (c >> 6)));
      else {
        if (c < 0x10000)
          out.push_back(static_cast<char>(0xe0 | (c >> 12)));
        else {
          out.push_back(static_cast<char>(0xf0 | (c >> 18)));
          out.push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
        }
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
      }
      out.push_back(static_cast<char>(0x80 | (c & 63)));
    }
  };
  for (size_t i = 0; i < bytes.size(); i += 8) {
    uint32_t l, r;
    std::memcpy(&l, bytes.data() + i, 4);
    std::memcpy(&r, bytes.data() + i + 4, 4);
    cipher.decrypt(l, r);
    uint8_t block[8];
    std::memcpy(block, &l, 4);
    std::memcpy(block + 4, &r, 4);
    // Native Decrypt2 uses little-endian words; text within each word is BE
    // UTF-16. Native Append consumes each four-unit block up to its first NUL.
    for (unsigned j = 0; j < 8; j += 2) {
      uint16_t u =
          static_cast<uint16_t>((uint16_t(block[j]) << 8) | block[j + 1]);
      if (!u)
        break;
      append(u);
    }
  }
  require(high == 0, "database connection truncated UTF-16 surrogate");
  return out;
}
} // namespace nwd::detail
