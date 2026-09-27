#pragma once
#include <string>
#include <string_view>
#include <cstdint>

namespace nwd::detail {
// ExtractUTF8String (not the NW10 variant) combines valid surrogate pairs and
// encodes unpaired surrogates as three bytes, rather than replacing them.
inline std::string xref_utf8(std::u16string_view text) {
  text = text.substr(0, text.find(u'\0'));
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    uint32_t c = text[i];
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.size() &&
        text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) {
      c = 0x10000 + ((c - 0xd800) << 10) + (text[++i] - 0xdc00);
    }
    if (c <= 0x7f) {
      out.push_back(static_cast<char>(c));
    } else if (c <= 0x7ff) {
      out.push_back(static_cast<char>(0xc0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 63)));
    } else {
      if (c > 0xffff) {
        out.push_back(static_cast<char>(0xf0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
      } else {
        out.push_back(static_cast<char>(0xe0 | (c >> 12)));
      }
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
      out.push_back(static_cast<char>(0x80 | (c & 63)));
    }
  }
  return out;
}
struct XRefString {
  std::u16string value;
  bool valid = true;
};
// LcUString::UTF8ToWideStringImpl, UTF-8 context only. This intentionally
// differs from strict Unicode: surrogate code points are retained, FFFE/FFFF
// are replaced, and an invalid continuation consumes the offending byte.
inline XRefString xref_string(std::string_view bytes,
                              bool saved_length = false) {
  if (!saved_length)
    bytes = bytes.substr(0, bytes.find('\0'));
  XRefString out;
  out.value.reserve(bytes.size());
  auto replacement = [&] {
    out.value.push_back(char16_t(0xfffd));
    out.valid = false;
  };
  size_t i = 0;
  while (i < bytes.size() && bytes[i] != '\0') {
    const auto lead = static_cast<uint8_t>(bytes[i++]);
    if (lead <= 0x7f) {
      out.value.push_back(lead);
      continue;
    }
    unsigned count;
    uint32_t value;
    if ((lead & 0xf8) == 0xf0) {
      count = 3;
      value = lead & 7;
    } else if ((lead & 0xf0) == 0xe0) {
      count = 2;
      value = lead & 15;
    } else if ((lead & 0xe0) == 0xc0) {
      count = 1;
      value = lead & 31;
    } else {
      replacement();
      continue;
    }
    if (count > bytes.size() - i) {
      replacement(); // native retries each remaining byte separately
      continue;
    }
    bool continuation = true;
    for (unsigned j = 0; j < count; ++j) {
      const auto next = static_cast<uint8_t>(bytes[i++]);
      if ((next & 0xc0) != 0x80) {
        continuation = false;
        break;
      }
      value = (value << 6) | (next & 63);
    }
    if (!continuation || value <= 0x7f || value > 0x10ffff || value == 0xfffe ||
        value == 0xffff ||
        (value <= 0x7ff    ? count != 1
         : value <= 0xffff ? count != 2
                           : count != 3)) {
      replacement();
      continue;
    }
    if (value < 0x10000) {
      out.value.push_back(static_cast<char16_t>(value));
    } else {
      value -= 0x10000;
      out.value.push_back(static_cast<char16_t>(0xd800 + (value >> 10)));
      out.value.push_back(static_cast<char16_t>(0xdc00 + (value & 1023)));
    }
  }
  return out;
}
} // namespace nwd::detail
