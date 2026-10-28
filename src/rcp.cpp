#include "internal.hpp"
#include "expat/nwd_expat_prefix.h"
#define XML_STATIC 1
#include "expat/lib/expat.h"
#include <climits>
#include <cstring>
#include <limits>
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

namespace nwd {
namespace {
uint16_t zip_u16(std::span<const uint8_t> b, uint64_t at) {
  detail::require(at <= b.size() && 2 <= b.size() - at,
                  "RCP ZIP field outside archive");
  return uint16_t(b[at]) | uint16_t(b[at + 1]) << 8;
}
uint32_t zip_u32(std::span<const uint8_t> b, uint64_t at) {
  detail::require(at <= b.size() && 4 <= b.size() - at,
                  "RCP ZIP field outside archive");
  return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 |
         uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}
uint64_t zip_u64(std::span<const uint8_t> b, uint64_t at) {
  return uint64_t(zip_u32(b, at)) | uint64_t(zip_u32(b, at + 4)) << 32;
}
void zip_range(std::span<const uint8_t> b, uint64_t at, uint64_t n,
               std::string_view what) {
  detail::require(at <= b.size() && n <= b.size() - at,
                  std::string("RCP ZIP ") + std::string(what) +
                      " outside archive");
}
uint32_t zip_crc32(std::span<const uint8_t> bytes) {
  uLong crc = crc32(0L, Z_NULL, 0);
  size_t at = 0;
  while (at < bytes.size()) {
    const auto count = std::min<size_t>(bytes.size() - at, UINT_MAX);
    crc = crc32(crc, bytes.data() + at, static_cast<uInt>(count));
    at += count;
  }
  return static_cast<uint32_t>(crc);
}
struct ZipDirectory {
  uint64_t entries = 0, size = 0, offset = 0;
};
ZipDirectory zip_directory(std::span<const uint8_t> b) {
  detail::require(b.size() >= 22, "RCP ZIP end record absent");
  const uint64_t begin = b.size() > 22 + 65535 ? b.size() - 22 - 65535 : 0;
  uint64_t eocd = UINT64_MAX;
  for (uint64_t at = b.size() - 22;; --at) {
    if (zip_u32(b, at) == 0x06054b50u) {
      const auto comment = zip_u16(b, at + 20);
      if (at + 22ull + comment == b.size()) {
        eocd = at;
        break;
      }
    }
    if (at == begin)
      break;
  }
  detail::require(eocd != UINT64_MAX, "RCP ZIP end record not found");
  const auto disk = zip_u16(b, eocd + 4);
  const auto cd_disk = zip_u16(b, eocd + 6);
  const auto entries_disk = zip_u16(b, eocd + 8);
  const auto entries_total = zip_u16(b, eocd + 10);
  const auto cd_size32 = zip_u32(b, eocd + 12);
  const auto cd_offset32 = zip_u32(b, eocd + 16);
  const bool needs_zip64 = entries_disk == 0xffff || entries_total == 0xffff ||
                           cd_size32 == 0xffffffffu ||
                           cd_offset32 == 0xffffffffu;
  ZipDirectory out;
  if (needs_zip64) {
    detail::require(eocd >= 20 && zip_u32(b, eocd - 20) == 0x07064b50u,
                    "RCP ZIP64 locator absent");
    const auto locator_disk = zip_u32(b, eocd - 16);
    const auto zip64_offset = zip_u64(b, eocd - 12);
    const auto disks = zip_u32(b, eocd - 4);
    zip_range(b, zip64_offset, 12, "ZIP64 end header");
    detail::require(locator_disk == 0 && disks == 1 &&
                        zip_u32(b, zip64_offset) == 0x06064b50u,
                    "multi-disk or invalid RCP ZIP64 archive");
    const auto record_size = zip_u64(b, zip64_offset + 4);
    detail::require(record_size <= UINT64_MAX - 12,
                    "RCP ZIP64 end record size overflow");
    zip_range(b, zip64_offset, 12 + record_size, "ZIP64 end record");
    detail::require(record_size >= 44, "short RCP ZIP64 end record");
    const auto disk64 = zip_u32(b, zip64_offset + 16);
    const auto cd_disk64 = zip_u32(b, zip64_offset + 20);
    const auto entries_disk64 = zip_u64(b, zip64_offset + 24);
    out.entries = zip_u64(b, zip64_offset + 32);
    out.size = zip_u64(b, zip64_offset + 40);
    out.offset = zip_u64(b, zip64_offset + 48);
    detail::require(disk64 == 0 && cd_disk64 == 0 &&
                        entries_disk64 == out.entries,
                    "multi-disk RCP ZIP64 archive");
  } else {
    detail::require(disk == 0 && cd_disk == 0 && entries_disk == entries_total,
                    "multi-disk RCP ZIP archive");
    out.entries = entries_total;
    out.size = cd_size32;
    out.offset = cd_offset32;
  }
  zip_range(b, out.offset, out.size, "central directory");
  return out;
}

void parse_zip64_extra(std::span<const uint8_t> extra, RcpArchiveEntry &entry,
                       bool need_sizes, bool need_offset, bool need_disk) {
  uint64_t at = 0;
  bool found = false;
  while (at < extra.size()) {
    detail::require(extra.size() - at >= 4, "truncated RCP ZIP extra field");
    const auto id = zip_u16(extra, at);
    const auto size = zip_u16(extra, at + 2);
    at += 4;
    zip_range(extra, at, size, "extra field");
    if (id == 0x0001) {
      found = true;
      uint64_t p = at;
      const auto end = at + size;
      if (need_sizes) {
        detail::require(end - p >= 16, "short RCP ZIP64 sizes");
        entry.uncompressed_size = zip_u64(extra, p);
        entry.compressed_size = zip_u64(extra, p + 8);
        entry.zip64_sizes = true;
        p += 16;
      }
      if (need_offset) {
        detail::require(end - p >= 8, "short RCP ZIP64 offset");
        entry.local_header_offset = zip_u64(extra, p);
        entry.zip64_offset = true;
        p += 8;
      }
      if (need_disk) {
        detail::require(end - p >= 4, "short RCP ZIP64 disk number");
        entry.disk_number_start = zip_u32(extra, p);
      }
      break;
    }
    at += size;
  }
  detail::require(found || !(need_sizes || need_offset || need_disk),
                  "RCP ZIP64 extra field absent");
}

struct XmlParser {
  XML_Parser xml = XML_ParserCreate(nullptr);
  RcpXmlOptions limits;
  std::vector<RcpXmlNode> nodes;
  std::vector<Id> stack, last_child;
  Id root = none, last_top = none;
  uint64_t text_bytes = 0, attribute_count = 0;
  std::exception_ptr error;
  explicit XmlParser(RcpXmlOptions options) : limits(options) {
    if (!xml)
      throw std::bad_alloc();
  }
  ~XmlParser() { XML_ParserFree(xml); }
  template <class F> void guard(F &&f) noexcept {
    if (error)
      return;
    try {
      f();
    } catch (...) {
      error = std::current_exception();
      XML_StopParser(xml, XML_FALSE);
    }
  }
  void account(size_t size) {
    detail::require(size <= limits.max_text_bytes - text_bytes,
                    "RCP XML text budget");
    text_bytes += size;
  }
  std::string copy(std::string_view value) {
    account(value.size());
    return std::string(value);
  }
  Id add(RcpXmlNodeKind kind, std::string_view name, std::string_view value) {
    detail::require(nodes.size() < limits.max_nodes && nodes.size() < none,
                    "RCP XML node budget");
    const Id id = static_cast<Id>(nodes.size());
    RcpXmlNode node;
    node.kind = kind;
    node.parent = stack.empty() ? none : stack.back();
    node.byte_offset = static_cast<uint64_t>(XML_GetCurrentByteIndex(xml));
    node.name = copy(name);
    node.value = copy(value);
    nodes.push_back(std::move(node));
    auto &previous = stack.empty() ? last_top : last_child.back();
    if (previous != none)
      nodes[previous].next_sibling = id;
    else if (!stack.empty())
      nodes[stack.back()].first_child = id;
    previous = id;
    return id;
  }
  static void XMLCALL start(void *arg, const XML_Char *name,
                            const XML_Char **attrs) noexcept {
    auto &p = *static_cast<XmlParser *>(arg);
    p.guard([&] {
      detail::require(p.stack.size() < p.limits.max_depth,
                      "RCP XML depth budget");
      const auto id = p.add(RcpXmlNodeKind::element, name, {});
      if (p.stack.empty()) {
        detail::require(p.root == none, "RCP XML has multiple roots");
        p.root = id;
      }
      for (size_t i = 0; attrs[i]; i += 2) {
        detail::require(p.attribute_count < p.limits.max_attributes,
                        "RCP XML attribute budget");
        ++p.attribute_count;
        p.nodes[id].attributes.push_back(
            {p.copy(attrs[i]), p.copy(attrs[i + 1])});
      }
      p.stack.push_back(id);
      p.last_child.push_back(none);
    });
  }
  static void XMLCALL end(void *arg, const XML_Char *) noexcept {
    auto &p = *static_cast<XmlParser *>(arg);
    p.guard([&] {
      detail::require(!p.stack.empty(), "RCP XML unexpected close tag");
      p.stack.pop_back();
      p.last_child.pop_back();
    });
  }
  static void XMLCALL text(void *arg, const XML_Char *value,
                           int size) noexcept {
    auto &p = *static_cast<XmlParser *>(arg);
    p.guard([&] {
      if (size <= 0)
        return;
      const auto text = std::string_view(value, static_cast<size_t>(size));
      const auto previous =
          p.stack.empty() ? p.last_top : p.last_child.back();
      if (previous != none && p.nodes[previous].kind == RcpXmlNodeKind::text) {
        p.account(text.size());
        p.nodes[previous].value.append(text);
      } else
        p.add(RcpXmlNodeKind::text, {}, text);
    });
  }
  static void XMLCALL comment(void *arg, const XML_Char *value) noexcept {
    auto &p = *static_cast<XmlParser *>(arg);
    p.guard([&] { p.add(RcpXmlNodeKind::comment, {}, value); });
  }
  static void XMLCALL instruction(void *arg, const XML_Char *name,
                                  const XML_Char *value) noexcept {
    auto &p = *static_cast<XmlParser *>(arg);
    p.guard([&] { p.add(RcpXmlNodeKind::instruction, name, value); });
  }
  static void XMLCALL doctype(void *arg, const XML_Char *, const XML_Char *,
                              const XML_Char *, int) noexcept {
    auto &p = *static_cast<XmlParser *>(arg);
    p.guard([] { throw Error("RCP XML DTD is unsupported"); });
  }
  void parse(std::span<const uint8_t> bytes) {
    detail::require(bytes.size() <= limits.max_bytes, "RCP XML input budget");
    XML_SetUserData(xml, this);
    XML_SetElementHandler(xml, start, end);
    XML_SetCharacterDataHandler(xml, text);
    XML_SetCommentHandler(xml, comment);
    XML_SetProcessingInstructionHandler(xml, instruction);
    XML_SetStartDoctypeDeclHandler(xml, doctype);
    XML_SetParamEntityParsing(xml, XML_PARAM_ENTITY_PARSING_NEVER);
    size_t at = 0;
    do {
      const auto count = std::min<size_t>(bytes.size() - at, 65536);
      const char *data = count
                             ? reinterpret_cast<const char *>(bytes.data() + at)
                             : "";
      const auto status = XML_Parse(xml, data, static_cast<int>(count),
                                    at + count == bytes.size());
      if (error)
        std::rethrow_exception(error);
      if (status != XML_STATUS_OK)
        throw Error("RCP XML at byte " +
                    std::to_string(XML_GetCurrentByteIndex(xml)) + ": " +
                    XML_ErrorString(XML_GetErrorCode(xml)));
      at += count;
    } while (at < bytes.size());
    detail::require(root != none && stack.empty(), "RCP XML root absent");
  }
};
} // namespace

struct RcpReader::Impl {
  const uint8_t *data = nullptr;
  size_t size = 0;
  std::vector<RcpArchiveEntry> entries;
#ifdef _WIN32
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping = nullptr;
#else
  int file = -1;
#endif
  ~Impl() {
#ifdef _WIN32
    if (data)
      UnmapViewOfFile(data);
    if (mapping)
      CloseHandle(mapping);
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
#else
    if (data)
      munmap(const_cast<uint8_t *>(data), size);
    if (file >= 0)
      close(file);
#endif
  }
  void open(const std::filesystem::path &path) {
#ifdef _WIN32
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      throw Error("cannot open RCP file");
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 22 ||
        static_cast<uint64_t>(length.QuadPart) >
            std::numeric_limits<size_t>::max())
      throw Error("invalid RCP file size");
    size = static_cast<size_t>(length.QuadPart);
    mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping)
      throw Error("cannot map RCP file");
    data = static_cast<const uint8_t *>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!data)
      throw Error("cannot map RCP file view");
#else
    file = ::open(path.c_str(), O_RDONLY);
    if (file < 0)
      throw Error("cannot open RCP file");
    struct stat info {};
    if (fstat(file, &info) != 0 || info.st_size < 22 ||
        static_cast<uint64_t>(info.st_size) >
            std::numeric_limits<size_t>::max())
      throw Error("invalid RCP file size");
    size = static_cast<size_t>(info.st_size);
    void *view = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file, 0);
    if (view == MAP_FAILED)
      throw Error("cannot map RCP file");
    data = static_cast<const uint8_t *>(view);
#endif
  }
  void index(RcpArchiveOptions options) {
    const auto bytes = std::span<const uint8_t>(data, size);
    const auto directory = zip_directory(bytes);
    detail::require(directory.entries <= options.max_entries &&
                        directory.entries <= UINT32_MAX,
                    "RCP ZIP entry count budget");
    uint64_t cursor = directory.offset;
    uint64_t total_uncompressed = 0;
    entries.reserve(static_cast<size_t>(directory.entries));
    for (uint64_t i = 0; i < directory.entries; ++i) {
      zip_range(bytes, cursor, 46, "central directory record");
      detail::require(zip_u32(bytes, cursor) == 0x02014b50u,
                      "invalid RCP ZIP central directory signature");
      RcpArchiveEntry entry;
      entry.version_made_by = zip_u16(bytes, cursor + 4);
      entry.version_needed = zip_u16(bytes, cursor + 6);
      entry.flags = zip_u16(bytes, cursor + 8);
      entry.compression_method = zip_u16(bytes, cursor + 10);
      entry.crc32 = zip_u32(bytes, cursor + 16);
      const auto comp32 = zip_u32(bytes, cursor + 20);
      const auto uncomp32 = zip_u32(bytes, cursor + 24);
      const auto name_size = zip_u16(bytes, cursor + 28);
      const auto extra_size = zip_u16(bytes, cursor + 30);
      const auto comment_size = zip_u16(bytes, cursor + 32);
      const auto disk16 = zip_u16(bytes, cursor + 34);
      entry.external_attributes = zip_u32(bytes, cursor + 38);
      const auto offset32 = zip_u32(bytes, cursor + 42);
      const uint64_t record_size = 46ull + name_size + extra_size + comment_size;
      zip_range(bytes, cursor, record_size, "central directory record");
      const auto name_at = cursor + 46;
      entry.name.assign(reinterpret_cast<const char *>(bytes.data() + name_at),
                        name_size);
      auto extra = bytes.subspan(static_cast<size_t>(name_at + name_size),
                                 extra_size);
      entry.central_extra.assign(extra.begin(), extra.end());
      const auto comment_at = name_at + name_size + extra_size;
      entry.central_comment.assign(
          bytes.begin() + static_cast<ptrdiff_t>(comment_at),
          bytes.begin() + static_cast<ptrdiff_t>(comment_at + comment_size));
      const bool need_sizes = comp32 == 0xffffffffu || uncomp32 == 0xffffffffu;
      const bool need_offset = offset32 == 0xffffffffu;
      const bool need_disk = disk16 == 0xffff;
      entry.compressed_size = comp32;
      entry.uncompressed_size = uncomp32;
      entry.local_header_offset = offset32;
      entry.disk_number_start = disk16;
      parse_zip64_extra(extra, entry, need_sizes, need_offset, need_disk);
      detail::require(entry.disk_number_start == 0,
                      "multi-disk RCP ZIP member");
      detail::require(entry.uncompressed_size <= options.max_entry_bytes,
                      "RCP ZIP member size budget");
      detail::require(entry.uncompressed_size <=
                          options.max_total_uncompressed_bytes -
                              total_uncompressed,
                      "RCP ZIP total expanded size budget");
      total_uncompressed += entry.uncompressed_size;
      entry.directory = !entry.name.empty() &&
                        (entry.name.back() == '/' || entry.name.back() == '\\');
      zip_range(bytes, entry.local_header_offset, 30, "local header");
      const auto local = entry.local_header_offset;
      detail::require(zip_u32(bytes, local) == 0x04034b50u,
                      "invalid RCP ZIP local header signature");
      const auto local_flags = zip_u16(bytes, local + 6);
      const auto local_method = zip_u16(bytes, local + 8);
      const auto local_name_size = zip_u16(bytes, local + 26);
      const auto local_extra_size = zip_u16(bytes, local + 28);
      detail::require(local_flags == entry.flags &&
                          local_method == entry.compression_method &&
                          local_name_size == name_size,
                      "RCP ZIP local/central header mismatch");
      zip_range(bytes, local + 30, uint64_t(local_name_size) + local_extra_size,
                "local name/extra");
      detail::require(std::equal(entry.name.begin(), entry.name.end(),
                                 reinterpret_cast<const char *>(
                                     bytes.data() + local + 30)),
                      "RCP ZIP local/central filename mismatch");
      entry.payload_offset = local + 30ull + local_name_size + local_extra_size;
      const auto local_extra_at = local + 30ull + local_name_size;
      entry.local_extra.assign(
          bytes.begin() + static_cast<ptrdiff_t>(local_extra_at),
          bytes.begin() +
              static_cast<ptrdiff_t>(local_extra_at + local_extra_size));
      zip_range(bytes, entry.payload_offset, entry.compressed_size,
                "member payload");
      entries.push_back(std::move(entry));
      cursor += record_size;
    }
    detail::require(cursor == directory.offset + directory.size,
                    "RCP ZIP central directory length mismatch");
  }
};

RcpReader::RcpReader(const std::filesystem::path &path,
                     RcpArchiveOptions options) {
  auto impl = std::make_shared<Impl>();
  impl->open(path);
  impl->index(options);
  impl_ = std::move(impl);
}
std::span<const RcpArchiveEntry> RcpReader::entries() const noexcept {
  return impl_->entries;
}
std::span<const uint8_t> RcpReader::raw_archive() const noexcept {
  return {impl_->data, impl_->size};
}
std::span<const uint8_t>
RcpReader::raw_compressed_entry(uint32_t entry_index) const {
  detail::require(entry_index < impl_->entries.size(),
                  "RCP ZIP entry index outside directory");
  const auto &entry = impl_->entries[entry_index];
  return {impl_->data + entry.payload_offset,
          static_cast<size_t>(entry.compressed_size)};
}
std::vector<uint8_t> RcpReader::read_entry(uint32_t entry_index) const {
  detail::require(entry_index < impl_->entries.size(),
                  "RCP ZIP entry index outside directory");
  const auto &entry = impl_->entries[entry_index];
  detail::require((entry.flags & 1u) == 0 && (entry.flags & 0x40u) == 0,
                  "encrypted RCP ZIP member unsupported");
  const auto input = raw_compressed_entry(entry_index);
  detail::require(entry.uncompressed_size <= SIZE_MAX,
                  "RCP ZIP member exceeds address space");
  std::vector<uint8_t> output(static_cast<size_t>(entry.uncompressed_size));
  if (entry.compression_method == 0) {
    detail::require(entry.compressed_size == entry.uncompressed_size,
                    "stored RCP ZIP member length mismatch");
    if (!input.empty())
      std::memcpy(output.data(), input.data(), input.size());
  } else if (entry.compression_method == 8) {
    detail::require(input.size() <= UINT_MAX && output.size() <= UINT_MAX,
                    "RCP ZIP DEFLATE member exceeds zlib limit");
    z_stream stream{};
    detail::require(inflateInit2(&stream, -MAX_WBITS) == Z_OK,
                    "RCP ZIP inflate initialization failed");
    struct EndInflate {
      z_stream *stream;
      ~EndInflate() { inflateEnd(stream); }
    } end{&stream};
    uint8_t empty_output = 0;
    stream.next_in = const_cast<Bytef *>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = output.empty() ? &empty_output : output.data();
    stream.avail_out = static_cast<uInt>(output.empty() ? 1 : output.size());
    const auto status = inflate(&stream, Z_FINISH);
    detail::require(status == Z_STREAM_END && stream.total_in == input.size() &&
                        stream.total_out == output.size(),
                    "invalid RCP ZIP DEFLATE stream");
  } else {
    throw Error("unsupported RCP ZIP compression method " +
                std::to_string(entry.compression_method));
  }
  const auto crc = zip_crc32(output);
  detail::require(crc == entry.crc32, "RCP ZIP member CRC mismatch");
  return output;
}

struct RcpXmlDocument::Impl {
  std::vector<RcpXmlNode> nodes;
  Id root = none;
  Impl(std::span<const uint8_t> bytes, RcpXmlOptions options) {
    XmlParser parser(options);
    parser.parse(bytes);
    nodes = std::move(parser.nodes);
    root = parser.root;
  }
};
RcpXmlDocument::RcpXmlDocument(std::span<const uint8_t> bytes,
                               RcpXmlOptions options)
    : impl_(std::make_shared<Impl>(bytes, options)) {}
std::span<const RcpXmlNode> RcpXmlDocument::nodes() const noexcept {
  return impl_->nodes;
}
Id RcpXmlDocument::root() const noexcept { return impl_->root; }
std::optional<std::string_view>
RcpXmlDocument::attribute(Id element, std::string_view name) const {
  detail::require(element < impl_->nodes.size() &&
                      impl_->nodes[element].kind == RcpXmlNodeKind::element,
                  "RCP XML attribute lookup requires an element");
  for (const auto &attribute : impl_->nodes[element].attributes)
    if (attribute.name == name)
      return attribute.value;
  return std::nullopt;
}
std::vector<Id> RcpXmlDocument::elements_named(std::string_view name) const {
  std::vector<Id> out;
  for (Id i = 0; i < impl_->nodes.size(); ++i)
    if (impl_->nodes[i].kind == RcpXmlNodeKind::element &&
        impl_->nodes[i].name == name)
      out.push_back(i);
  return out;
}
std::string RcpXmlDocument::text_content(Id element, uint64_t max_bytes) const {
  detail::require(element < impl_->nodes.size() &&
                      impl_->nodes[element].kind == RcpXmlNodeKind::element,
                  "RCP XML text lookup requires an element");
  std::string out;
  std::vector<Id> pending{impl_->nodes[element].first_child};
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (id == none)
      continue;
    const auto &node = impl_->nodes[id];
    if (node.kind == RcpXmlNodeKind::text) {
      detail::require(node.value.size() <= max_bytes - out.size(),
                      "RCP XML text result budget");
      out += node.value;
    }
    if (node.next_sibling != none)
      pending.push_back(node.next_sibling);
    if (node.first_child != none)
      pending.push_back(node.first_child);
  }
  return out;
}

namespace {
std::optional<Id> rcp_child_element(const RcpXmlDocument &xml, Id parent,
                                    std::string_view name) {
  const auto nodes = xml.nodes();
  if (parent >= nodes.size())
    return std::nullopt;
  for (Id child = nodes[parent].first_child; child != none;
       child = nodes[child].next_sibling)
    if (nodes[child].kind == RcpXmlNodeKind::element &&
        nodes[child].name == name)
      return child;
  return std::nullopt;
}
std::optional<std::string_view> rcp_child_value(const RcpXmlDocument &xml,
                                               Id parent,
                                               std::string_view name) {
  const auto child = rcp_child_element(xml, parent, name);
  return child ? xml.attribute(*child, "Value") : std::nullopt;
}
std::array<std::optional<std::string_view>, 3>
rcp_vector_fields(const RcpXmlDocument &xml, Id parent, std::string_view name) {
  std::array<std::optional<std::string_view>, 3> out;
  if (const auto child = rcp_child_element(xml, parent, name)) {
    constexpr std::string_view axes[] = {"x", "y", "z"};
    for (size_t i = 0; i < 3; ++i)
      out[i] = xml.attribute(*child, axes[i]);
  }
  return out;
}
template <class Records, class GetId>
std::unordered_map<std::string_view, std::vector<uint32_t>>
rcp_identity_index(const Records &records, GetId get_id) {
  std::unordered_map<std::string_view, std::vector<uint32_t>> result;
  for (uint32_t i = 0; i < records.size(); ++i)
    if (const auto id = get_id(records[i]); id && !id->empty())
      result[*id].push_back(i);
  return result;
}
template <class SourceRecords, class TargetIndex, class GetId, class SetLink>
void rcp_link_identities(const SourceRecords &sources,
                         const TargetIndex &source_index,
                         const TargetIndex &target_index, GetId get_id,
                         SetLink set_link) {
  for (uint32_t i = 0; i < sources.size(); ++i) {
    const auto id = get_id(sources[i]);
    if (!id || id->empty()) {
      set_link(i, RcpIdentityLinkStatus::missing_id, std::nullopt);
      continue;
    }
    const auto source = source_index.find(*id);
    const auto target = target_index.find(*id);
    if (target == target_index.end()) {
      set_link(i, RcpIdentityLinkStatus::no_match, std::nullopt);
      continue;
    }
    if (source == source_index.end() || source->second.size() != 1 ||
        target->second.size() != 1) {
      set_link(i, RcpIdentityLinkStatus::ambiguous, std::nullopt);
      continue;
    }
    set_link(i, RcpIdentityLinkStatus::unique, target->second.front());
  }
}
} // namespace

struct RcpProjectDocument::Impl {
  RcpXmlDocument xml;
  std::optional<std::string_view> project_uuid;
  std::vector<RcpProjectVoxelTreeRuntime> runtime;
  std::vector<RcpProjectScan> scans;
  Impl(std::span<const uint8_t> bytes, RcpXmlOptions options)
      : xml(bytes, options) {
    const auto project_ids = xml.elements_named("ProjectId");
    if (project_ids.size() == 1)
      project_uuid = xml.attribute(project_ids.front(), "UUID");
    for (const auto node : xml.elements_named("VoxelTreeRunTime")) {
      RcpProjectVoxelTreeRuntime record;
      record.xml_node = node;
      record.id = xml.attribute(node, "Id");
      record.node_name = rcp_child_value(xml, node, "NodeName");
      record.group = rcp_child_value(xml, node, "Group");
      record.path = rcp_child_value(xml, node, "Path");
      record.relative_path = rcp_child_value(xml, node, "RelativePath");
      record.num_points = rcp_child_value(xml, node, "NumPoints");
      record.translation = rcp_vector_fields(xml, node, "Translation");
      record.rotation = rcp_vector_fields(xml, node, "Rotation");
      record.scale = rcp_vector_fields(xml, node, "Scale");
      runtime.push_back(std::move(record));
    }
    for (const auto node : xml.elements_named("Scan")) {
      RcpProjectScan record;
      record.xml_node = node;
      record.id = xml.attribute(node, "ID");
      record.title = xml.attribute(node, "Title");
      record.virtual_scan = xml.attribute(node, "VirtualScan");
      record.visible = xml.attribute(node, "Visible");
      record.isolate_visible = xml.attribute(node, "IsolateVisible");
      scans.push_back(std::move(record));
    }
    const auto runtime_index = rcp_identity_index(
        runtime, [](const auto &record) { return record.id; });
    const auto scan_index =
        rcp_identity_index(scans, [](const auto &record) { return record.id; });
    rcp_link_identities(
        runtime, runtime_index, scan_index,
        [](const auto &record) { return record.id; },
        [&](uint32_t i, RcpIdentityLinkStatus status,
            std::optional<uint32_t> target) {
          runtime[i].scan_link = status;
          runtime[i].scan_index = target;
        });
    rcp_link_identities(
        scans, scan_index, runtime_index,
        [](const auto &record) { return record.id; },
        [&](uint32_t i, RcpIdentityLinkStatus status,
            std::optional<uint32_t> target) {
          scans[i].runtime_link = status;
          scans[i].voxel_tree_runtime_index = target;
        });
  }
};
RcpProjectDocument::RcpProjectDocument(std::span<const uint8_t> bytes,
                                       RcpXmlOptions options)
    : impl_(std::make_shared<Impl>(bytes, options)) {}
const RcpXmlDocument &RcpProjectDocument::xml() const noexcept {
  return impl_->xml;
}
std::optional<std::string_view>
RcpProjectDocument::project_uuid() const noexcept {
  return impl_->project_uuid;
}
std::span<const RcpProjectVoxelTreeRuntime>
RcpProjectDocument::voxel_tree_runtime() const noexcept {
  return impl_->runtime;
}
std::span<const RcpProjectScan> RcpProjectDocument::scans() const noexcept {
  return impl_->scans;
}
} // namespace nwd
