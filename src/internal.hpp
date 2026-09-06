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
struct UnsupportedLayout : Error {
  using Error::Error;
};
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
template <class StringReader, class ReferenceReader>
Value read_data_value(Cursor &r, StringReader string_reader,
                      ReferenceReader reference_reader) {
  Value v;
  v.tag = r.u32();
  switch (v.tag) {
  case 0:
    break;
  case 1:
  case 6:
  case 7:
  case 10:
  case 11:
    v.number[0] = r.read<double>();
    break;
  case 2:
  case 3:
    v.integer = r.read<int32_t>();
    break;
  case 4:
  case 9:
    v.string = string_reader();
    break;
  case 5:
    v.integer = static_cast<int64_t>(r.u64());
    break;
  case 8:
    v.reference = reference_reader();
    break;
  case 12:
    for (auto &x : v.number)
      x = r.read<double>();
    break;
  case 13:
    v.number[0] = r.read<double>();
    v.number[1] = r.read<double>();
    break;
  default:
    r.fail("unsupported property variant " + std::to_string(v.tag));
  }
  return v;
}
template <class ReferenceReader>
Appearance read_appearance(Cursor &r, ReferenceReader reference) {
  Appearance a;
  a.flags = r.u32();
  if (a.flags & 1)
    a.material = reference(54);
  if (a.flags & 32)
    a.asset = reference(185);
  constexpr std::array<uint32_t, 4> bits{64, 2, 4, 8};
  for (unsigned i = 0; i < bits.size(); ++i)
    if (a.flags & bits[i])
      a.overrides[i] = r.u32();
  return a;
}
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
void read_nwf_appearances(NwfData &, std::span<const uint8_t>, uint32_t,
                          const Options &);
void decode_external_payload(ExternalGeometry &,
                             std::span<const SchemaDefinition>, uint32_t);
class ObjectReader {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  ObjectReader(std::span<const uint8_t>, ObjectGraph &, uint32_t, Options = {});
  ~ObjectReader();
  Id object(Cursor &);
  Id string(Cursor &);
};
void read_source_references(Cursor &, std::vector<NwfReference> &,
                            ObjectReader &, uint32_t version,
                            const Options &options = {});
CurrentView read_current_view(Cursor &, uint32_t version);
ExternalReferenceTable read_xref_table(Cursor &, const Options &);
void read_texture_spaces(Cursor &, std::vector<NwfTextureSpace> &, uint32_t,
                         bool implicit_node_map, const Options &);
void read_cache_data(Cursor &, std::vector<CachePlugin> &,
                     std::vector<CachedReference> &, std::vector<CacheOption> &,
                     ObjectReader &, const Options &, uint64_t &budget);
void read_file_database(FileDatabase &, Cursor &, bool wrapped,
                        const Options &);
CurrentView read_viewpoint(Cursor &, uint32_t version);
Camera read_camera(Cursor &, uint32_t version);
void read_clip_planes(Cursor &, std::vector<ViewFields> &, ViewFields &,
                      uint32_t version);
SchemaInstance read_schema_instance(Cursor &,
                                    std::span<const SchemaDefinition>);
void read_partition(Model &, std::span<const uint8_t>, uint32_t,
                    const Options &);
void read_metadata(Model &, std::span<const uint8_t>,
                   const std::vector<Chunk> &, uint32_t, const Options &,
                   std::vector<bool> &);
inline double elapsed(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}
} // namespace nwd::detail
