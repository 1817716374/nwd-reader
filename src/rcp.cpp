#include "internal.hpp"
#include "expat/nwd_expat_prefix.h"
#define XML_STATIC 1
#include "expat/lib/expat.h"
#include <climits>
#include <cstring>
#include <map>
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
bool zip_valid_utf8(std::string_view value) {
  size_t i = 0;
  while (i < value.size()) {
    const auto first = static_cast<uint8_t>(value[i]);
    if (first <= 0x7f) {
      ++i;
      continue;
    }
    uint32_t codepoint = 0;
    size_t count = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      codepoint = first & 0x1f;
      count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      codepoint = first & 0x0f;
      count = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      codepoint = first & 0x07;
      count = 3;
    } else {
      return false;
    }
    if (count > value.size() - i - 1)
      return false;
    for (size_t j = 1; j <= count; ++j) {
      const auto byte = static_cast<uint8_t>(value[i + j]);
      if ((byte & 0xc0) != 0x80)
        return false;
      codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    if ((count == 2 && codepoint < 0x800) ||
        (count == 3 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
        codepoint > 0x10ffff)
      return false;
    i += count + 1;
  }
  return true;
}
void zip_append_utf8(std::string &out, uint32_t cp) {
  if (cp <= 0x7f) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}
std::string zip_decode_cp437(std::span<const uint8_t> bytes) {
  static constexpr std::array<char32_t, 128> upper = {
      0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7,
      0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
      0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
      0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
      0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba,
      0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
      0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
      0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
      0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
      0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
      0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
      0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
      0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4,
      0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
      0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
      0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0};
  std::string result;
  result.reserve(bytes.size());
  for (const auto byte : bytes)
    zip_append_utf8(result, byte < 0x80 ? byte : upper[byte - 0x80]);
  return result;
}
std::optional<std::string>
zip_unicode_extra(std::span<const uint8_t> extra,
                  std::span<const uint8_t> raw_text, uint16_t wanted_id) {
  std::optional<std::string> result;
  uint64_t at = 0;
  while (at < extra.size()) {
    detail::require(extra.size() - at >= 4,
                    "truncated RCP ZIP Unicode text extra field");
    const auto id = zip_u16(extra, at);
    const auto size = zip_u16(extra, at + 2);
    at += 4;
    detail::require(size <= extra.size() - at,
                    "truncated RCP ZIP Unicode text payload");
    const auto field = extra.subspan(static_cast<size_t>(at), size);
    if (id == wanted_id && size >= 5 && field[0] == 1 &&
        zip_u32(field, 1) == zip_crc32(raw_text)) {
      const std::string_view text(
          reinterpret_cast<const char *>(field.data() + 5), size - 5);
      if (zip_valid_utf8(text)) {
        if (result && *result != text)
          return std::nullopt;
        result = std::string(text);
      }
    }
    at += size;
  }
  return result;
}
bool zip_has_extra(std::span<const uint8_t> extra, uint16_t wanted) {
  uint64_t at = 0;
  while (at < extra.size()) {
    detail::require(extra.size() - at >= 4,
                    "truncated RCP ZIP extra field");
    const auto id = zip_u16(extra, at);
    const auto size = zip_u16(extra, at + 2);
    at += 4;
    detail::require(size <= extra.size() - at,
                    "truncated RCP ZIP extra payload");
    if (id == wanted)
      return true;
    at += size;
  }
  return false;
}
std::pair<std::string, RcpTextEncoding>
zip_decode_name(std::span<const uint8_t> raw_name, uint16_t flags,
                std::span<const uint8_t> central_extra,
                std::span<const uint8_t> local_extra) {
  const std::string_view raw(reinterpret_cast<const char *>(raw_name.data()),
                             raw_name.size());
  if (flags & 0x0800) {
    detail::require(zip_valid_utf8(raw), "invalid UTF-8 RCP ZIP filename");
    return {std::string(raw), RcpTextEncoding::utf8_flag};
  }
  if (auto name = zip_unicode_extra(central_extra, raw_name, 0x7075))
    return {std::move(*name), RcpTextEncoding::unicode_path_extra};
  if (auto name = zip_unicode_extra(local_extra, raw_name, 0x7075))
    return {std::move(*name), RcpTextEncoding::unicode_path_extra};
  const bool unknown_legacy_encoding =
      zip_has_extra(central_extra, 0x0008) ||
      zip_has_extra(local_extra, 0x0008);
  return {zip_decode_cp437(raw_name),
          unknown_legacy_encoding ? RcpTextEncoding::unknown_legacy_encoding
                                  : RcpTextEncoding::cp437};
}
std::pair<std::string, RcpTextEncoding>
zip_decode_comment(std::span<const uint8_t> raw_comment, uint16_t flags,
                   std::span<const uint8_t> central_extra) {
  const char *raw_data = raw_comment.empty()
                             ? ""
                             : reinterpret_cast<const char *>(raw_comment.data());
  const std::string_view raw(raw_data, raw_comment.size());
  if (flags & 0x0800) {
    detail::require(zip_valid_utf8(raw),
                    "invalid UTF-8 RCP ZIP comment");
    return {std::string(raw), RcpTextEncoding::utf8_flag};
  }
  if (auto comment = zip_unicode_extra(central_extra, raw_comment, 0x6375))
    return {std::move(*comment), RcpTextEncoding::unicode_comment_extra};
  const bool unknown_legacy_encoding = zip_has_extra(central_extra, 0x0008);
  return {zip_decode_cp437(raw_comment),
          unknown_legacy_encoding ? RcpTextEncoding::unknown_legacy_encoding
                                  : RcpTextEncoding::cp437};
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
      entry.raw_name.assign(bytes.begin() + static_cast<ptrdiff_t>(name_at),
                            bytes.begin() +
                                static_cast<ptrdiff_t>(name_at + name_size));
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
      detail::require(std::equal(entry.raw_name.begin(), entry.raw_name.end(),
                                 bytes.begin() +
                                     static_cast<ptrdiff_t>(local + 30)),
                      "RCP ZIP local/central filename mismatch");
      entry.payload_offset = local + 30ull + local_name_size + local_extra_size;
      const auto local_extra_at = local + 30ull + local_name_size;
      entry.local_extra.assign(
          bytes.begin() + static_cast<ptrdiff_t>(local_extra_at),
          bytes.begin() +
              static_cast<ptrdiff_t>(local_extra_at + local_extra_size));
      auto decoded_name = zip_decode_name(entry.raw_name, entry.flags,
                                          entry.central_extra,
                                          entry.local_extra);
      entry.name = std::move(decoded_name.first);
      entry.name_encoding = decoded_name.second;
      auto decoded_comment = zip_decode_comment(
          entry.central_comment, entry.flags, entry.central_extra);
      entry.comment = std::move(decoded_comment.first);
      entry.comment_encoding = decoded_comment.second;
      entry.directory = !entry.name.empty() &&
                        (entry.name.back() == '/' || entry.name.back() == '\\');
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
std::vector<Id> rcp_child_elements(const RcpXmlDocument &xml, Id parent,
                                  std::string_view name) {
  std::vector<Id> result;
  const auto nodes = xml.nodes();
  if (parent >= nodes.size())
    return result;
  for (Id child = nodes[parent].first_child; child != none;
       child = nodes[child].next_sibling)
    if (nodes[child].kind == RcpXmlNodeKind::element &&
        nodes[child].name == name)
      result.push_back(child);
  return result;
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
RcpProjectVector3 rcp_project_vector3(const RcpXmlDocument &xml, Id parent,
                                     std::string_view name) {
  RcpProjectVector3 out;
  if (const auto child = rcp_child_element(xml, parent, name)) {
    constexpr std::string_view lowercase[] = {"x", "y", "z"};
    constexpr std::string_view uppercase[] = {"X", "Y", "Z"};
    for (size_t i = 0; i < 3; ++i) {
      out.lowercase_xyz[i] = xml.attribute(*child, lowercase[i]);
      out.uppercase_xyz[i] = xml.attribute(*child, uppercase[i]);
    }
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
  std::vector<RcpProjectShot> shots;
  std::vector<RcpProjectGroup> groups;
  std::vector<RcpProjectRegionLayer> region_layers;
  std::vector<RcpProjectViewState> navigator_view_states;
  std::vector<RcpProjectSavedView> saved_view_states;
  Impl(std::span<const uint8_t> bytes, RcpXmlOptions options)
      : xml(bytes, options) {
    const auto root = xml.root();
    if (const auto project_id = rcp_child_element(xml, root, "ProjectId"))
      project_uuid = xml.attribute(*project_id, "UUID");
    if (const auto point_cloud =
            rcp_child_element(xml, root, "PointCloudSegmentList")) {
      if (const auto manager =
              rcp_child_element(xml, *point_cloud, "RegionManager")) {
        const auto append_region_layers = [&](Id container, bool is_default) {
          for (const auto node : rcp_child_elements(xml, container, "Layer")) {
            RcpProjectRegionLayer record;
            record.xml_node = node;
            record.id = xml.attribute(node, "Id");
            record.name = xml.attribute(node, "Name");
            record.index = xml.attribute(node, "Index");
            record.visible = xml.attribute(node, "Visible");
            record.locked = xml.attribute(node, "Locked");
            record.removed = xml.attribute(node, "Removed");
            record.default_layer = is_default;
            if (const auto color = rcp_child_element(xml, node, "Color"))
              record.color_node = *color;
            if (const auto bounds =
                    rcp_child_element(xml, node, "SelectionBounds"))
              record.selection_bounds_node = *bounds;
            region_layers.push_back(std::move(record));
          }
        };
        if (const auto default_layer =
                rcp_child_element(xml, *manager, "DefaultLayer"))
          append_region_layers(*default_layer, true);
        if (const auto region_array =
                rcp_child_element(xml, *manager, "RegionArray"))
          append_region_layers(*region_array, false);
      }
    }
    if (const auto nodes = rcp_child_element(xml, root, "Nodes"))
      for (const auto node :
           rcp_child_elements(xml, *nodes, "VoxelTreeRunTime")) {
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
    if (const auto navigator =
            rcp_child_element(xml, root, "ProjectNavigator")) {
      if (const auto scan_container =
              rcp_child_element(xml, *navigator, "Scans"))
        for (const auto node :
             rcp_child_elements(xml, *scan_container, "Scan")) {
          RcpProjectScan record;
          record.xml_node = node;
          record.id = xml.attribute(node, "ID");
          record.title = xml.attribute(node, "Title");
          record.virtual_scan = xml.attribute(node, "VirtualScan");
          record.visible = xml.attribute(node, "Visible");
          record.isolate_visible = xml.attribute(node, "IsolateVisible");
          scans.push_back(std::move(record));
        }
      if (const auto states = rcp_child_element(xml, *navigator, "States"))
        for (const auto node :
             rcp_child_elements(xml, *states, "ViewState")) {
          RcpProjectViewState record;
          record.xml_node = node;
          record.id = xml.attribute(node, "ID");
          record.title = xml.attribute(node, "Title");
          navigator_view_states.push_back(std::move(record));
        }
    }
    if (const auto project = rcp_child_element(xml, root, "Project")) {
      for (const auto node : rcp_child_elements(xml, *project, "ShotInfo")) {
        RcpProjectShot record;
        record.xml_node = node;
        record.id = xml.attribute(node, "id");
        record.name = xml.attribute(node, "name");
        record.point_cloud_id = xml.attribute(node, "PCPid");
        record.has_intensity = xml.attribute(node, "HasIntensity");
        record.path = xml.attribute(node, "path");
        record.relative_path = xml.attribute(node, "relativePath");
        record.raw_scan_path = xml.attribute(node, "rawScanPath");
        record.relative_raw_scan_path =
            xml.attribute(node, "relativeRawScanPath");
        record.type = xml.attribute(node, "type");
        shots.push_back(std::move(record));
      }
      if (const auto group_info =
              rcp_child_element(xml, *project, "GroupInfo"))
        for (const auto node :
             rcp_child_elements(xml, *group_info, "GroupInfo"))
          groups.push_back({node, xml.attribute(node, "id")});
    }
    if (const auto views = rcp_child_element(xml, root, "ViewStates"))
      for (const auto node : rcp_child_elements(xml, *views, "ViewState")) {
        RcpProjectSavedView record;
        record.xml_node = node;
        record.id = xml.attribute(node, "ID");
        record.title = xml.attribute(node, "Title");
        record.fov = xml.attribute(node, "FOV");
        record.point_size = xml.attribute(node, "PointSize");
        record.shading_mode = xml.attribute(node, "ShadingMode");
        if (const auto child = rcp_child_element(xml, node, "Camera")) {
          record.camera_node = *child;
          record.camera_eye = rcp_project_vector3(xml, *child, "Eye");
          record.camera_center = rcp_project_vector3(xml, *child, "Center");
          record.camera_up = rcp_project_vector3(xml, *child, "Up");
        }
        if (const auto child = rcp_child_element(xml, node, "LimitBox"))
          record.limit_box_node = *child;
        if (const auto child =
                rcp_child_element(xml, node, "ClippingBoundaries"))
          record.clipping_boundaries_node = *child;
        if (const auto child = rcp_child_element(xml, node, "Regions"))
          record.regions_node = *child;
        if (record.regions_node != none) {
          record.regions_isolated =
              xml.attribute(record.regions_node, "Isolated");
          for (const auto status :
               rcp_child_elements(xml, record.regions_node, "Status")) {
            RcpProjectRegionStatus region_status;
            region_status.xml_node = status;
            region_status.id = xml.attribute(status, "ID");
            region_status.visible = xml.attribute(status, "Visible");
            record.regions.push_back(std::move(region_status));
          }
        }
        if (const auto child =
                rcp_child_element(xml, node, "DefaultRegionStatus"))
          record.default_region_status_node = *child;
        if (record.default_region_status_node != none) {
          for (const auto status : rcp_child_elements(
                   xml, record.default_region_status_node, "Status")) {
            RcpProjectRegionStatus region_status;
            region_status.xml_node = status;
            region_status.id = xml.attribute(status, "ID");
            region_status.visible = xml.attribute(status, "Visible");
            record.default_region_status.push_back(std::move(region_status));
          }
        }
        if (const auto child = rcp_child_element(xml, node, "ScanLocations")) {
          record.scan_locations_node = *child;
          for (const auto status :
               rcp_child_elements(xml, *child, "Status")) {
            RcpProjectViewScanStatus scan_status;
            scan_status.xml_node = status;
            scan_status.id = xml.attribute(status, "ID");
            scan_status.visible = xml.attribute(status, "Visible");
            record.scan_locations.push_back(std::move(scan_status));
          }
        }
        if (const auto child =
                rcp_child_element(xml, node, "BackgroundColor"))
          record.background_color_node = *child;
        saved_view_states.push_back(std::move(record));
      }

    const auto runtime_index = rcp_identity_index(
        runtime, [](const auto &record) { return record.id; });
    const auto scan_index =
        rcp_identity_index(scans, [](const auto &record) { return record.id; });
    const auto region_layer_index = rcp_identity_index(
        region_layers, [](const auto &record) { return record.id; });
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
    const auto shot_index =
        rcp_identity_index(shots, [](const auto &record) { return record.id; });
    const auto shot_runtime_index = rcp_identity_index(
        shots, [](const auto &record) { return record.point_cloud_id; });
    const auto group_index =
        rcp_identity_index(groups, [](const auto &record) { return record.id; });
    const auto navigator_view_index = rcp_identity_index(
        navigator_view_states, [](const auto &record) { return record.id; });
    const auto saved_view_index = rcp_identity_index(
        saved_view_states, [](const auto &record) { return record.id; });
    rcp_link_identities(
        shots, shot_runtime_index, runtime_index,
        [](const auto &record) { return record.point_cloud_id; },
        [&](uint32_t i, RcpIdentityLinkStatus status,
            std::optional<uint32_t> target) {
          shots[i].runtime_link = status;
          shots[i].voxel_tree_runtime_index = target;
        });
    rcp_link_identities(
        shots, shot_index, group_index,
        [](const auto &record) { return record.id; },
        [&](uint32_t i, RcpIdentityLinkStatus status,
            std::optional<uint32_t> target) {
          shots[i].group_link = status;
          shots[i].group_index = target;
        });
    rcp_link_identities(
        navigator_view_states, navigator_view_index, saved_view_index,
        [](const auto &record) { return record.id; },
        [&](uint32_t i, RcpIdentityLinkStatus status,
            std::optional<uint32_t> target) {
          navigator_view_states[i].saved_view_link = status;
          navigator_view_states[i].saved_view_index = target;
        });
    for (auto &view : saved_view_states) {
      const auto status_index = rcp_identity_index(
          view.scan_locations,
          [](const auto &record) { return record.id; });
      rcp_link_identities(
          view.scan_locations, status_index, scan_index,
          [](const auto &record) { return record.id; },
          [&](uint32_t i, RcpIdentityLinkStatus status,
              std::optional<uint32_t> target) {
            view.scan_locations[i].scan_link = status;
            view.scan_locations[i].scan_index = target;
          });
      const auto link_region_statuses = [&](auto &statuses) {
        const auto source_index = rcp_identity_index(
            statuses, [](const auto &record) { return record.id; });
        rcp_link_identities(
            statuses, source_index, region_layer_index,
            [](const auto &record) { return record.id; },
            [&](uint32_t i, RcpIdentityLinkStatus status,
                std::optional<uint32_t> target) {
              statuses[i].region_link = status;
              statuses[i].region_layer_index = target;
            });
      };
      link_region_statuses(view.default_region_status);
      link_region_statuses(view.regions);
    }
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
std::span<const RcpProjectShot> RcpProjectDocument::shots() const noexcept {
  return impl_->shots;
}
std::span<const RcpProjectGroup> RcpProjectDocument::groups() const noexcept {
  return impl_->groups;
}
std::span<const RcpProjectRegionLayer>
RcpProjectDocument::region_layers() const noexcept {
  return impl_->region_layers;
}
std::span<const RcpProjectViewState>
RcpProjectDocument::navigator_view_states() const noexcept {
  return impl_->navigator_view_states;
}
std::span<const RcpProjectSavedView>
RcpProjectDocument::saved_view_states() const noexcept {
  return impl_->saved_view_states;
}

namespace {
bool rcp_xml_name(std::string_view name) {
  if (name.size() < 4 || name[name.size() - 4] != '.')
    return false;
  constexpr std::string_view extension = ".xml";
  for (size_t i = 0; i < extension.size(); ++i) {
    auto c = name[name.size() - extension.size() + i];
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
    if (c != extension[i])
      return false;
  }
  return true;
}
uint32_t rcp_xml_member(const RcpReader &archive,
                        const RcpProjectOptions &options) {
  const auto entries = archive.entries();
  if (options.xml_member_index) {
    detail::require(*options.xml_member_index < entries.size(),
                    "RCP project XML member index outside archive");
    detail::require(!entries[*options.xml_member_index].directory,
                    "RCP project XML member is a directory");
    return *options.xml_member_index;
  }
  std::optional<uint32_t> result;
  for (uint32_t i = 0; i < entries.size(); ++i) {
    if (entries[i].directory || !rcp_xml_name(entries[i].name))
      continue;
    detail::require(!result, "RCP archive has multiple XML members");
    result = i;
  }
  detail::require(result.has_value(), "RCP archive has no XML member");
  return *result;
}
RcpProjectDocument rcp_load_project_document(
    const RcpReader &archive, const RcpProjectOptions &options) {
  auto bytes = archive.read_entry(rcp_xml_member(archive, options));
  return RcpProjectDocument(bytes, options.xml);
}
std::string rcp_native_separators(std::string_view text) {
  std::string result(text);
  std::replace(result.begin(), result.end(), '\\', '/');
  return result;
}
bool rcp_windows_absolute(std::string_view path) {
  return path.size() >= 3 &&
         ((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) &&
         path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}
std::optional<std::filesystem::path>
rcp_absolute_regular_file(const std::filesystem::path &path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec)
    return std::nullopt;
  auto absolute = std::filesystem::canonical(path, ec);
  if (!ec)
    return absolute;
  ec.clear();
  absolute = std::filesystem::absolute(path, ec);
  if (ec)
    return path.lexically_normal();
  return absolute.lexically_normal();
}
struct RcpPathResolution {
  RcpExternalRcsStatus status = RcpExternalRcsStatus::missing;
  std::filesystem::path path;
  std::string diagnostic;
};
RcpPathResolution rcp_choose_file(
    std::span<const std::filesystem::path> candidates,
    std::string_view candidate_description) {
  std::vector<std::filesystem::path> found;
  for (const auto &candidate : candidates) {
    const auto actual = rcp_absolute_regular_file(candidate);
    if (actual && std::find(found.begin(), found.end(), *actual) == found.end())
      found.push_back(*actual);
  }
  if (found.empty())
    return {RcpExternalRcsStatus::missing, {},
            std::string(candidate_description) + " not found"};
  if (found.size() != 1)
    return {RcpExternalRcsStatus::ambiguous, {},
            std::string(candidate_description) + " matched multiple files"};
  return {RcpExternalRcsStatus::loaded, std::move(found.front()), {}};
}
RcpPathResolution rcp_resolve_external(
    const RcpProjectVoxelTreeRuntime &runtime,
    const std::filesystem::path &project_directory,
    std::span<const std::filesystem::path> search_paths,
    std::span<const RcpPathRemap> remaps) {
  auto by_saved_path = [&](std::string_view saved) {
    std::vector<std::filesystem::path> mapped;
    for (const auto &remap : remaps)
      if (remap.saved_path == saved)
        mapped.push_back(remap.local_path);
    if (mapped.empty())
      return RcpPathResolution{RcpExternalRcsStatus::missing, {}, {}};
    auto result = rcp_choose_file(mapped, "explicit RCP path remap");
    if (result.status == RcpExternalRcsStatus::missing)
      result.diagnostic = "explicit RCP path remap target is missing";
    return result;
  };
  if (runtime.relative_path && !runtime.relative_path->empty()) {
    auto mapped = by_saved_path(*runtime.relative_path);
    if (!mapped.diagnostic.empty() || mapped.status == RcpExternalRcsStatus::loaded ||
        mapped.status == RcpExternalRcsStatus::ambiguous)
      return mapped;
  }
  if (runtime.path && !runtime.path->empty()) {
    auto mapped = by_saved_path(*runtime.path);
    if (!mapped.diagnostic.empty() || mapped.status == RcpExternalRcsStatus::loaded ||
        mapped.status == RcpExternalRcsStatus::ambiguous)
      return mapped;
  }

  if (runtime.relative_path && !runtime.relative_path->empty()) {
    const auto text = *runtime.relative_path;
    const std::filesystem::path relative(rcp_native_separators(text));
    if (relative.is_absolute() || !rcp_windows_absolute(text)) {
      const std::array candidates{project_directory / relative};
      auto direct = rcp_choose_file(candidates, "RCP RelativePath");
      if (direct.status == RcpExternalRcsStatus::loaded)
        return direct;
    }
  }
  if (runtime.path && !runtime.path->empty()) {
    const auto text = *runtime.path;
    const std::filesystem::path saved(rcp_native_separators(text));
    if (saved.is_absolute()) {
      const std::array candidates{saved};
      auto direct = rcp_choose_file(candidates, "RCP absolute Path");
      if (direct.status == RcpExternalRcsStatus::loaded)
        return direct;
    }
  }

  std::vector<std::filesystem::path> relative_candidates;
  std::vector<std::filesystem::path> saved_candidates;
  auto add_search_candidates = [&](std::optional<std::string_view> saved,
                                   std::vector<std::filesystem::path> &out) {
    if (!saved || saved->empty())
      return;
    const auto text = *saved;
    const std::filesystem::path path(rcp_native_separators(text));
    if (path.is_absolute() || rcp_windows_absolute(text))
      return;
    for (const auto &root : search_paths)
      out.push_back(root / path);
  };
  add_search_candidates(runtime.relative_path, relative_candidates);
  if (!relative_candidates.empty()) {
    auto result = rcp_choose_file(relative_candidates,
                                  "RCP RelativePath in search directories");
    if (result.status != RcpExternalRcsStatus::missing)
      return result;
  }
  add_search_candidates(runtime.path, saved_candidates);
  if (!saved_candidates.empty()) {
    auto result = rcp_choose_file(saved_candidates,
                                  "RCP Path in search directories");
    if (result.status != RcpExternalRcsStatus::missing)
      return result;
  }

  auto add_filename_candidates = [&](std::optional<std::string_view> saved,
                                     std::vector<std::filesystem::path> &out) {
    if (!saved || saved->empty())
      return;
    const std::filesystem::path path(rcp_native_separators(*saved));
    if (path.filename().empty())
      return;
    for (const auto &root : search_paths)
      out.push_back(root / path.filename());
  };
  std::vector<std::filesystem::path> filename_candidates;
  add_filename_candidates(runtime.relative_path, filename_candidates);
  add_filename_candidates(runtime.path, filename_candidates);
  if (!filename_candidates.empty()) {
    auto result = rcp_choose_file(filename_candidates,
                                  "RCP external filename fallback");
    if (result.status != RcpExternalRcsStatus::missing)
      return result;
  }
  return {RcpExternalRcsStatus::missing, {},
          "RCP external RCS path did not resolve"};
}
} // namespace

struct RcpProjectReader::Impl {
  RcpReader archive;
  RcpProjectDocument project;
  std::vector<RcpExternalRcsFile> external;
  Impl(const std::filesystem::path &path, RcpProjectOptions options)
      : archive(path, options.archive),
        project(rcp_load_project_document(archive, options)) {
    std::error_code ec;
    auto project_file = std::filesystem::absolute(path, ec);
    if (ec)
      project_file = path;
    const auto directory = project_file.parent_path();
    std::map<std::filesystem::path, std::shared_ptr<const RcsReader>> cache;
    const auto runtime = project.voxel_tree_runtime();
    external.reserve(runtime.size());
    for (uint32_t i = 0; i < runtime.size(); ++i) {
      const auto &record = runtime[i];
      RcpExternalRcsFile file;
      file.runtime_index = i;
      file.saved_relative_path = record.relative_path;
      file.saved_path = record.path;
      if (!options.load_external_rcs) {
        file.status = RcpExternalRcsStatus::disabled;
        file.diagnostic = "external RCS loading disabled by options";
        external.push_back(std::move(file));
        continue;
      }
      if ((!record.relative_path || record.relative_path->empty()) &&
          (!record.path || record.path->empty())) {
        file.status = RcpExternalRcsStatus::missing_path;
        file.diagnostic = "runtime record has no saved RCS path";
        external.push_back(std::move(file));
        continue;
      }
      auto resolved = rcp_resolve_external(record, directory,
                                           options.search_paths,
                                           options.remaps);
      file.status = resolved.status;
      file.resolved_path = std::move(resolved.path);
      file.diagnostic = std::move(resolved.diagnostic);
      if (file.status == RcpExternalRcsStatus::loaded) {
        const auto existing = cache.find(file.resolved_path);
        if (existing != cache.end())
          file.rcs = existing->second;
        else {
          try {
            auto mapped = std::make_shared<const RcsReader>(
                file.resolved_path, options.max_rcs_voxels,
                options.max_rcs_other_data_fields);
            cache.emplace(file.resolved_path, mapped);
            file.rcs = std::move(mapped);
          } catch (const std::exception &error) {
            file.status = RcpExternalRcsStatus::invalid;
            file.diagnostic = std::string("RCS load failed: ") + error.what();
          }
        }
      }
      external.push_back(std::move(file));
    }
  }
};
RcpProjectReader::RcpProjectReader(const std::filesystem::path &path,
                                   RcpProjectOptions options)
    : impl_(std::make_shared<Impl>(path, std::move(options))) {}
const RcpReader &RcpProjectReader::archive() const noexcept {
  return impl_->archive;
}
const RcpProjectDocument &RcpProjectReader::project() const noexcept {
  return impl_->project;
}
std::span<const RcpExternalRcsFile>
RcpProjectReader::external_rcs_files() const noexcept {
  return impl_->external;
}
} // namespace nwd
