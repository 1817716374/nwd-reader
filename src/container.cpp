#include "internal.hpp"
#include "blowfish.hpp"
#include <fstream>
#include <string_view>
#include <zlib.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace nwd::detail {
Inflated inflate_one(std::span<const uint8_t> input, uint64_t limit,
                     size_t hint) {
  require(input.size() <= UINT32_MAX,
          "compressed stream exceeds zlib input limit");
  require(limit > 0, "zero decoded limit");
  z_stream z{};
  require(inflateInit(&z) == Z_OK, "inflateInit failed");
  struct End {
    z_stream *z;
    ~End() { inflateEnd(z); }
  } end{&z};
  z.next_in = const_cast<Bytef *>(input.data());
  z.avail_in = static_cast<uInt>(input.size());
  Inflated out;
  out.bytes.resize(static_cast<size_t>(
      std::min<uint64_t>(limit, std::max<size_t>(hint, 256))));
  for (;;) {
    size_t used = z.total_out;
    if (used == out.bytes.size()) {
      require(used < limit, "decoded chunk limit exceeded");
      out.bytes.resize(
          static_cast<size_t>(std::min<uint64_t>(limit, used * 2)));
    }
    z.next_out = out.bytes.data() + used;
    z.avail_out = static_cast<uInt>(
        std::min<size_t>(UINT32_MAX, out.bytes.size() - used));
    int rc = inflate(&z, Z_NO_FLUSH);
    if (rc == Z_STREAM_END)
      break;
    require(rc == Z_OK,
            "zlib integrity/decompression error " + std::to_string(rc));
    require(z.avail_in || z.avail_out == 0, "truncated zlib stream");
  }
  out.consumed = z.total_in;
  out.bytes.resize(z.total_out);
  return out;
}
std::vector<Bytes> chunk_blocks(std::span<const uint8_t> file, const Chunk &c,
                                const Options &o) {
  require(c.flags == 1, "unsupported chunk flags: " + c.name);
  require(c.prefix_bytes <= c.size, "invalid chunk prefix");
  auto data = file.subspan(static_cast<size_t>(c.offset + c.prefix_bytes),
                           static_cast<size_t>(c.size - c.prefix_bytes));
  std::vector<Bytes> blocks;
  uint64_t total = 0;
  while (!data.empty()) {
    auto x = inflate_one(
        data, o.max_decoded_chunk - total,
        std::max<size_t>(65536, std::min<size_t>(data.size() * 4, 64 << 20)));
    total += x.bytes.size();
    require(x.consumed != 0, "empty compressed stream");
    data = data.subspan(x.consumed);
    blocks.emplace_back(std::move(x.bytes));
  }
  return blocks;
}
} // namespace nwd::detail
namespace nwd {
struct Document::Impl {
  Options options;
  std::span<const uint8_t> file;
  std::string header;
  uint32_t version = 0;
  std::vector<Chunk> chunks;
  double container_ms = 0;
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE, mapping = nullptr;
  ~Impl() {
    if (file.data())
      UnmapViewOfFile(file.data());
    if (mapping)
      CloseHandle(mapping);
    if (handle != INVALID_HANDLE_VALUE)
      CloseHandle(handle);
  }
#else
  int fd = -1;
  ~Impl() {
    if (file.data())
      munmap(const_cast<uint8_t *>(file.data()), file.size());
    if (fd >= 0)
      close(fd);
  }
#endif
};
Document::Document(const std::filesystem::path &path, Options options)
    : impl_(std::make_shared<Impl>()) {
  using namespace detail;
  auto start = std::chrono::steady_clock::now();
  auto &d = *impl_;
  d.options = options;
#ifdef _WIN32
  d.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  require(d.handle != INVALID_HANDLE_VALUE, "cannot open input");
  LARGE_INTEGER size{};
  require(GetFileSizeEx(d.handle, &size) && size.QuadPart > 64,
          "input too small");
  d.mapping =
      CreateFileMappingW(d.handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
  require(d.mapping != nullptr, "file mapping failed");
  auto p = static_cast<const uint8_t *>(
      MapViewOfFile(d.mapping, FILE_MAP_READ, 0, 0, 0));
  require(p != nullptr, "mapping view failed");
  d.file = {p, static_cast<size_t>(size.QuadPart)};
#else
  d.fd = open(path.c_str(), O_RDONLY);
  require(d.fd >= 0, "cannot open input");
  struct stat st{};
  require(fstat(d.fd, &st) == 0 && st.st_size > 64, "input too small");
  void *p = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, d.fd, 0);
  require(p != MAP_FAILED, "mmap failed");
  d.file = {static_cast<const uint8_t *>(p), static_cast<size_t>(st.st_size)};
#endif
  std::string_view text(reinterpret_cast<const char *>(d.file.data()),
                        d.file.size());
  require(text.starts_with("#LcUStream-V1.1"),
          "not a supported LcUStream container");
  if (text.substr(0, 52).find("lichunk-007") != std::string_view::npos) {
    uint64_t directory_end;
    std::memcpy(&directory_end, d.file.data() + 52, 8);
    require(directory_end > 132 && directory_end < d.file.size() - 8,
            "legacy directory extent");
    size_t line = text.find('\n', 60);
    require(line != std::string_view::npos && line < 316,
            "legacy directory header");
    d.header = std::string(text.substr(60, line - 60));
    size_t v = d.header.find("navisworks:");
    require(v != std::string::npos, "missing legacy version");
    d.version = std::stoul(d.header.substr(v + 11));
    size_t z = (line + 4) & ~size_t(3);
    while (
        z < line + 32 && z + 1 < directory_end &&
        !((d.file[z] & 15) == 8 && (d.file[z] * 256 + d.file[z + 1]) % 31 == 0))
      ++z;
    require(z < directory_end && z < line + 32, "missing legacy zlib header");
    auto decoded = inflate_one(d.file.subspan(z, directory_end - z), 64 << 20);
    require(z + decoded.consumed == directory_end,
            "legacy directory compressed boundary");
    Cursor r(decoded.bytes, "legacy directory");
    require(r.u32() == UINT32_MAX, "legacy directory marker");
    auto count = r.u32();
    require(count > 0 && count < 1000000, "legacy directory count");
    for (uint32_t i = 0; i < count; ++i) {
      Chunk c;
      c.name = r.string();
      c.flags = r.u32();
      d.chunks.push_back(std::move(c));
    }
    r.exact();
    require(directory_end + 8 + uint64_t(count) * 16 + 8 <= d.file.size(),
            "legacy extent table overflow");
    for (uint32_t i = 0; i < count; ++i) {
      auto p = d.file.data() + directory_end + 8 + size_t(i) * 16;
      auto &c = d.chunks[i];
      std::memcpy(&c.offset, p, 8);
      std::memcpy(&c.prefix_bytes, p + 8, 4);
      std::memcpy(&c.index_bytes, p + 12, 4);
      uint64_t next;
      std::memcpy(&next, p + 16, 8);
      require(next >= c.offset && next <= d.file.size(), "legacy chunk extent");
      c.size = next - c.offset;
      require(c.prefix_bytes <= c.size &&
                  c.index_bytes <= c.size - c.prefix_bytes,
              "legacy chunk prefix/index extent");
    }
    require(d.chunks.front().offset ==
                    directory_end + 8 + uint64_t(count) * 16 + 8 &&
                d.chunks.back().offset + d.chunks.back().size == d.file.size(),
            "legacy extent table coverage");
    d.container_ms = elapsed(start);
    return;
  }
  require(text.substr(0, 52).find("lichunk-008") != std::string_view::npos,
          "unsupported container revision");
  size_t footer = text.rfind("#LcUStream");
  require(footer != std::string_view::npos && footer >= 16 &&
              footer + 52 == d.file.size() &&
              text.substr(footer, 52) == text.substr(0, 52),
          "missing directory footer");
  uint64_t at;
  std::memcpy(&at, d.file.data() + footer - 8, 8);
  require(at >= 52 && at < footer - 16, "directory pointer outside file");
  size_t line = text.find('\n', static_cast<size_t>(at));
  require(line != std::string_view::npos && line < at + 256 &&
              line < footer - 16,
          "invalid directory header");
  d.header = std::string(text.substr(at, line - at));
  size_t v = d.header.find("navisworks:");
  require(v != std::string::npos, "missing Navisworks version");
  d.version = static_cast<uint32_t>(std::stoul(d.header.substr(v + 11)));
  size_t z = line + 1;
  for (; z + 1 < std::min<size_t>(line + 32, footer - 8); ++z)
    if ((d.file[z] & 15) == 8 && ((d.file[z] * 256 + d.file[z + 1]) % 31) == 0)
      break;
  require(z + 1 < std::min<size_t>(line + 32, footer - 8),
          "missing directory zlib header");
  auto inflated = inflate_one(d.file.subspan(z, footer - 8 - z), 64 << 20);
  require(z + inflated.consumed == footer - 16,
          "directory trailer must be 8 bytes (opaque, not validated)");
  Cursor r(inflated.bytes, "directory");
  require(r.u32() == UINT32_MAX, "directory marker");
  uint32_t count = r.u32();
  require(count < 1000000, "unreasonable directory size");
  uint64_t previous = 52;
  for (uint32_t i = 0; i < count; ++i) {
    Chunk c;
    c.name = r.string();
    c.flags = r.u32();
    c.offset = r.u64();
    c.size = r.u64();
    c.prefix_bytes = r.u32();
    if (c.prefix_bytes)
      c.index_bytes = r.u32();
    r.align(8);
    require(c.offset == previous && c.offset <= at && c.size <= at - c.offset,
            "directory chunk extent mismatch");
    require(c.prefix_bytes <= c.size &&
                c.index_bytes <= c.size - c.prefix_bytes,
            "chunk prefix/index extent");
    previous = c.offset + c.size;
    d.chunks.push_back(std::move(c));
  }
  require(previous == at, "directory gap at end");
  r.exact();
  d.container_ms = elapsed(start);
}
uint32_t Document::version() const { return impl_->version; }
const Options &Document::options() const { return impl_->options; }
const std::string &Document::header() const { return impl_->header; }
const std::vector<Chunk> &Document::chunks() const { return impl_->chunks; }
std::vector<uint8_t> Document::read_chunk(size_t index) const {
  using namespace detail;
  require(index < impl_->chunks.size(), "chunk index outside directory");
  auto &c = impl_->chunks[index];
  if (c.flags != 1)
    return Bytes(impl_->file.begin() + c.offset,
                 impl_->file.begin() + c.offset + c.size);
  auto blocks = chunk_blocks(impl_->file, c, impl_->options);
  if (blocks.size() == 1)
    return std::move(blocks.front());
  Bytes out;
  size_t total = 0;
  for (auto &b : blocks)
    total += b.size();
  out.reserve(total);
  for (auto &b : blocks)
    out.insert(out.end(), b.begin(), b.end());
  return out;
}
std::vector<uint8_t> Document::read_product_payload(size_t index) const {
  using namespace detail;
  const auto &c = chunks().at(index);
  if (c.flags == 3 && (c.name.ends_with("LcOpNwdSerial") ||
                       c.name.ends_with("LcOpNwdGeometryCompress"))) {
    require(!c.prefix_bytes && !c.index_bytes, "cipher chunk envelope");
    auto compressed = decode_chunk_cipher(impl_->file.subspan(c.offset, c.size),
                                          options().max_decoded_chunk);
    auto decoded = inflate_one(compressed, options().max_decoded_chunk);
    require(decoded.consumed == compressed.size(), "cipher compressed extent");
    return std::move(decoded.bytes);
  }
  if (!c.name.ends_with("LcOpFileDatabaseElementNWD"))
    return read_chunk(index);
  require(c.flags == 1 && c.prefix_bytes == 8 && c.index_bytes > 0,
          "unsupported database page envelope");
  Cursor prefix(impl_->file.subspan(c.offset, 8));
  auto segment_size = prefix.u32(), size = prefix.u32();
  require(segment_size > 0 && size > 0 && size <= options().max_decoded_chunk,
          "database page sizes/resource limit");
  auto idx = inflate_one(
      impl_->file.subspan(c.offset + c.size - c.index_bytes, c.index_bytes),
      options().max_decoded_chunk);
  require(idx.consumed == c.index_bytes, "database index compressed extent");
  Cursor r(idx.bytes);
  auto n = r.u32();
  require(n == (uint64_t(size) + segment_size - 1) / segment_size &&
              n <= options().max_objects,
          "database segment count");
  std::vector<uint32_t> sizes;
  std::vector<uint64_t> offsets{c.offset + c.prefix_bytes};
  for (uint32_t j = 0; j < n; ++j) {
    sizes.push_back(r.u32());
    offsets.push_back(offsets.back() + sizes.back());
    require(offsets.back() <= c.offset + c.size - c.index_bytes,
            "database segment extent");
  }
  r.exact();
  require(offsets.back() == c.offset + c.size - c.index_bytes,
          "database indexed compressed lengths");
  Bytes out(size);
  parallel_for(n, options().threads, [&](size_t j) {
    const auto expected =
        std::min<uint64_t>(segment_size, size - j * uint64_t(segment_size));
    auto block =
        inflate_one(impl_->file.subspan(offsets[j], sizes[j]), expected);
    require(block.consumed == sizes[j] && block.bytes.size() == expected,
            "database decoded segment extent");
    std::copy(block.bytes.begin(), block.bytes.end(),
              out.begin() + j * uint64_t(segment_size));
  });
  return out;
}
Scene Document::read_scene() const {
  using namespace detail;
  auto start = std::chrono::steady_clock::now();
  Scene out;
  out.version = impl_->version;
  out.header = impl_->header;
  out.chunks = impl_->chunks;
  out.parsed_chunks.resize(out.chunks.size(), false);
  out.timing.container_ms = impl_->container_ms;
  bool schemas_seen = false;
  for (size_t i = 0; i < out.chunks.size(); ++i)
    if (out.chunks[i].name == "LcOpCommonSchemas") {
      require(!schemas_seen, "duplicate common schema table");
      schemas_seen = true;
      out.schemas = decode_schemas(read_chunk(i), version());
      out.parsed_chunks[i] = true;
    }
  for (auto &c : out.chunks)
    if (c.name == "LcOpNwdGeometry" || c.name.ends_with("\\LcOpNwdGeometry")) {
      Model m;
      m.name = c.name == "LcOpNwdGeometry"
                   ? ""
                   : c.name.substr(0, c.name.rfind('\\'));
      auto phase = std::chrono::steady_clock::now();
      read_geometry(m, impl_->file, c, impl_->options);
      size_t external_count = 0;
      for (auto &g : m.geometries)
        if (g.external) {
          ++external_count;
          auto e = std::make_shared<ExternalGeometry>(*g.external);
          decode_external_payload(*e, out.schemas, version());
          g.external = std::move(e);
        }
      if (external_count)
        out.warnings.push_back(
            m.name + ": " + std::to_string(external_count) +
            " external geometry descriptors retained; point/mesh coordinates "
            "not decoded from external descriptors; schema properties and "
            "bounds decoded");
      out.parsed_chunks[static_cast<size_t>(&c - out.chunks.data())] = true;
      size_t invalid_uv = 0;
      for (const auto &g : m.geometries)
        for (const auto &a : g.attributes)
          if (!a.finite)
            ++invalid_uv;
      if (invalid_uv)
        out.warnings.push_back(m.name + ": " + std::to_string(invalid_uv) +
                               " source UV arrays contain non-finite ranges; "
                               "values retained and flagged");
      out.timing.geometry_ms += elapsed(phase);
      std::string prefix = m.name.empty() ? "" : m.name + "\\";
      auto find = [&](std::string_view suffix) -> size_t {
        for (size_t i = 0; i < out.chunks.size(); ++i)
          if (out.chunks[i].name == prefix + std::string(suffix)) {
            out.parsed_chunks[i] = true;
            return i;
          }
        throw Error("missing chunk " + std::string(suffix));
      };
      phase = std::chrono::steady_clock::now();
      auto raw = read_chunk(find("LcOpNwdFragments"));
      read_instances(m, raw, version(), impl_->options);
      raw.clear();
      raw.shrink_to_fit();
      out.timing.instances_ms += elapsed(phase);
      if (impl_->options.metadata) {
        phase = std::chrono::steady_clock::now();
        read_metadata(m, impl_->file, out.chunks, version(), impl_->options,
                      out.parsed_chunks);
        for (Id schema : m.schema_references)
          require(schema == none || schema < out.schemas.size(),
                  "partition schema reference outside global table");
        out.timing.metadata_ms += elapsed(phase);
        for (auto &instance : m.instances)
          require(instance.path < m.paths.size(),
                  "instance path outside logical hierarchy");
      }
      out.models.emplace_back(std::move(m));
    }
  require(!out.models.empty(),
          "no embedded scene geometry; external references may be required");
  if (impl_->options.viewpoints) {
    auto phase = std::chrono::steady_clock::now();
    for (size_t i = 0; i < out.chunks.size(); ++i) {
      const auto &name = out.chunks[i].name;
      if (name == "LcOpCurrentViewElement" ||
          name.ends_with("\\LcOpCurrentViewElement")) {
        auto v = decode_current_view(read_chunk(i), version());
        v.chunk_name = name;
        out.current_views.push_back(std::move(v));
        out.parsed_chunks[i] = true;
      }
    }
    out.timing.viewpoints_ms = elapsed(phase);
  }
  if (impl_->options.resources) {
    auto phase = std::chrono::steady_clock::now();
    for (size_t i = 0; i < out.chunks.size(); ++i)
      if (out.chunks[i].name.starts_with("nwd:")) {
        out.resources.push_back(read_resource(i));
        out.parsed_chunks[i] = true;
      }
    out.timing.resources_ms = elapsed(phase);
  }
  if (impl_->options.products) {
    auto products = std::make_shared<ProductData>(read_products());
    for (size_t i = 0; i < products->blocks.size(); ++i) {
      auto &b = products->blocks[i];
      auto *tree = std::get_if<SpatialHierarchy>(&b.value);
      if (!tree || b.status != ProductStatus::decoded)
        continue;
      for (size_t m = 0; m < out.models.size(); ++m) {
        const auto &model = out.models[m];
        const auto chunk_name = (model.name.empty() ? "" : model.name + "\\") +
                                "LcOpNwdSpatialHierarchy";
        if (out.chunks[i].name != chunk_name)
          continue;
        require(tree->model == none, "ambiguous spatial model namespace");
        tree->model = static_cast<Id>(m);
        tree->associations_verified = std::all_of(
            tree->nodes.begin(), tree->nodes.end(), [&](const SpatialNode &n) {
              return (n.type != 1 && n.type != 4) ||
                     n.fragment < model.instances.size();
            });
      }
      if (!tree->associations_verified) {
        b.status = ProductStatus::partial;
        b.diagnostic = "spatial fragment association missing/outside model";
      }
    }
    out.products = std::move(products);
    for (size_t i = 0; i < out.chunks.size(); ++i)
      if (out.products->blocks[i].status == ProductStatus::decoded)
        out.parsed_chunks[i] = true;
  }
  out.timing.total_ms = elapsed(start) + out.timing.container_ms;
  return out;
}
EmbeddedResource Document::read_resource(size_t index) const {
  using namespace detail;
  require(index < impl_->chunks.size() &&
              impl_->chunks[index].name.starts_with("nwd:"),
          "not an embedded resource chunk");
  auto data = read_chunk(index);
  Cursor r(data, "embedded resource");
  EmbeddedResource resource;
  resource.name = impl_->chunks[index].name;
  auto size = r.u64();
  resource.block_size_hint = r.u32();
  auto bytes = r.raw(static_cast<size_t>(size));
  resource.bytes.assign(bytes.begin(), bytes.end());
  r.exact();
  return resource;
}
} // namespace nwd
