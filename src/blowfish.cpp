#include "internal.hpp"
#include "blowfish.hpp"
#include "blowfish_constants.hpp"
namespace nwd::detail {
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
