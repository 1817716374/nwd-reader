#pragma once
#include "nwd/reader.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
namespace nwd::detail {
static_assert(std::endian::native == std::endian::little && sizeof(size_t) >= 8,
              "nwd_reader requires a 64-bit little-endian target");
using Bytes = std::vector<uint8_t>;
inline void require(bool condition, std::string_view why) {
  if (!condition)
    throw Error(std::string(why));
}
class Cursor {
public:
  std::span<const uint8_t> data;
  size_t pos = 0;
  std::string label;
  Cursor(std::span<const uint8_t> d, std::string l = "stream")
      : data(d), label(std::move(l)) {}
  [[noreturn]] void fail(const std::string &msg) const {
    throw Error(label + " @ " + std::to_string(pos) + ": " + msg);
  }
  void align(size_t n) {
    size_t p = (pos + n - 1) & ~(n - 1);
    if (p > data.size())
      fail("alignment outside stream");
    pos = p;
  }
  std::span<const uint8_t> raw(size_t n) {
    if (n > data.size() - pos)
      fail("truncated field");
    auto s = data.subspan(pos, n);
    pos += n;
    return s;
  }
  template <class T> T read() {
    align(sizeof(T) >= 8 ? 8 : 4);
    auto b = raw(sizeof(T));
    T v;
    std::memcpy(&v, b.data(), sizeof(T));
    return v;
  }
  uint32_t u32() { return read<uint32_t>(); }
  uint64_t u64() { return read<uint64_t>(); }
  double f64() {
    double v = read<double>();
    if (!std::isfinite(v))
      fail("non-finite number");
    return v;
  }
  float f32() {
    float v = read<float>();
    if (!std::isfinite(v))
      fail("non-finite coordinate");
    return v;
  }
  uint8_t byte() { return raw(1)[0]; }
  std::string string() {
    uint32_t n = u32();
    if (n == UINT32_MAX)
      return {};
    auto s = raw(n);
    while (!s.empty() && s.back() == 0)
      s = s.first(s.size() - 1);
    return {reinterpret_cast<const char *>(s.data()), s.size()};
  }
  void exact() {
    if (data.size() - pos >= 8)
      fail("unconsumed bytes");
    for (auto x : data.subspan(pos))
      if (x)
        fail("nonzero record tail");
  }
};
inline EmbeddedAssetFile read_embedded_asset(Cursor &r, std::string name,
                                             Id owner, uint32_t ordinal) {
  EmbeddedAssetFile file;
  file.name = std::move(name);
  file.owner = owner;
  file.ordinal = ordinal;
  file.prefix = r.string();
  file.suffix = r.string();
  auto size = r.u64();
  auto bytes =
      r.raw(size); // validate against remaining chunk before allocation
  file.bytes.assign(bytes.begin(), bytes.end());
  return file;
}
template <class F> void parallel_for(size_t count, unsigned threads, F fn) {
  if (!count)
    return;
  unsigned n = threads ? threads : std::thread::hardware_concurrency();
  n = std::max(1u, std::min<unsigned>(
                       n, static_cast<unsigned>(std::min<size_t>(count, 64))));
  if (n == 1) {
    for (size_t i = 0; i < count; ++i)
      fn(i);
    return;
  }
  std::atomic<size_t> next{0};
  std::atomic<bool> stop{false};
  std::exception_ptr error;
  std::mutex lock;
  std::vector<std::jthread> workers;
  const size_t grain = count < 1024 ? 1 : 16;
  for (unsigned t = 0; t < n; ++t)
    workers.emplace_back([&] {
      try {
        for (;;) {
          size_t start = next.fetch_add(grain);
          if (start >= count || stop.load())
            break;
          for (size_t i = start; i < std::min(count, start + grain); ++i)
            fn(i);
        }
      } catch (...) {
        stop = true;
        std::lock_guard guard(lock);
        if (!error)
          error = std::current_exception();
      }
    });
  for (auto &t : workers)
    t.join();
  if (error)
    std::rethrow_exception(error);
}
struct Inflated {
  Bytes bytes;
  size_t consumed = 0;
};
Inflated inflate_one(std::span<const uint8_t> input, uint64_t limit,
                     size_t hint = 65536);
std::vector<Bytes> chunk_blocks(std::span<const uint8_t> file,
                                const Chunk &chunk, const Options &options);
void read_geometry(Model &, std::span<const uint8_t>, const Chunk &,
                   const Options &);
void read_instances(Model &, std::span<const uint8_t>, uint32_t,
                    const Options &);
void read_nwf_appearances(NwfData&,std::span<const uint8_t>,uint32_t,const Options&);
void read_metadata(Model &, std::span<const uint8_t>,
                   const std::vector<Chunk> &, uint32_t, const Options &,
                   std::vector<bool> &);
inline double elapsed(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}
} // namespace nwd::detail
