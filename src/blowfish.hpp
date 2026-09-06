#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
namespace nwd::detail {
// Internal format decoder; not a general-purpose encryption API.
class Blowfish {
  std::array<uint32_t, 18> p_;
  std::array<std::array<uint32_t, 256>, 4> s_;
  uint32_t f(uint32_t) const;
  void encrypt(uint32_t &, uint32_t &) const;

public:
  explicit Blowfish(std::span<const uint8_t> key);
  void decrypt(uint32_t &, uint32_t &) const;
};
std::string decode_database_connection(std::span<const uint8_t>);
} // namespace nwd::detail
