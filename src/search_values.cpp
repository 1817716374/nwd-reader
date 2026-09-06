#include "internal.hpp"
#include <bit>
namespace nwd {
namespace {
bool floating(uint32_t tag) { return tag <= 11 && ((0xcc2u >> tag) & 1); }
using Status = SearchValueStatus;
Status result(bool match) { return match ? Status::match : Status::no_match; }
std::optional<std::string_view> source_string(const ObjectGraph &g, Id id) {
  if (id == none)
    return {};
  auto text = resolve_string(g, id);
  return text.substr(0, text.find('\0'));
}
std::optional<std::u16string> utf16(std::string_view s, uint64_t limit) {
  std::u16string out;
  out.reserve(static_cast<size_t>(std::min<uint64_t>(s.size(), limit)));
  for (size_t i = 0; i < s.size();) {
    uint32_t c = static_cast<uint8_t>(s[i++]);
    unsigned extra = 0;
    uint32_t minimum = 0;
    if (c >= 0xc2 && c <= 0xdf) {
      extra = 1;
      minimum = 0x80;
      c &= 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
      extra = 2;
      minimum = 0x800;
      c &= 0xf;
    } else if (c >= 0xf0 && c <= 0xf4) {
      extra = 3;
      minimum = 0x10000;
      c &= 7;
    } else if (c >= 0x80)
      return {};
    if (extra > s.size() - i)
      return {};
    for (unsigned n = 0; n < extra; ++n) {
      auto next = static_cast<uint8_t>(s[i++]);
      if ((next & 0xc0) != 0x80)
        return {};
      c = (c << 6) | (next & 0x3f);
    }
    if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
      return {};
    auto units = c > 0xffff ? 2u : 1u;
    detail::require(out.size() <= limit && units <= limit - out.size(),
                    "search text resource limit");
    if (units == 2) {
      c -= 0x10000;
      out.push_back(static_cast<char16_t>(0xd800 + (c >> 10)));
      out.push_back(static_cast<char16_t>(0xdc00 + (c & 0x3ff)));
    } else
      out.push_back(static_cast<char16_t>(c));
  }
  return out;
}
std::optional<bool> name_equal(SearchValueView a, SearchValueView b,
                               uint64_t max_text_units) {
  auto ar = a.value.reference, br = b.value.reference;
  if (ar.object == none || br.object == none)
    return ar.object == br.object;
  if (a.graphs.empty() || b.graphs.empty())
    return {};
  detail::require(ar.graph < a.graphs.size() && br.graph < b.graphs.size(),
                  "search name graph outside source");
  const auto &ag = a.graphs[ar.graph], &bg = b.graphs[br.graph];
  detail::require(ar.object < ag.objects.size() &&
                      br.object < bg.objects.size(),
                  "search name outside graph");
  const auto &an = ag.objects[ar.object], &bn = bg.objects[br.object];
  if ((an.type != 52 && an.type != 82) || (bn.type != 52 && bn.type != 82) ||
      an.strings.size() != 2 || bn.strings.size() != 2)
    return {};
  auto ai = source_string(ag, an.strings[1]),
       bi = source_string(bg, bn.strings[1]);
  // Older native name loading can repair malformed UTF-8 internal names.
  // Without that source context, do not report a guessed comparison result.
  if ((ai && !utf16(*ai, max_text_units)) ||
      (bi && !utf16(*bi, max_text_units)))
    return {};
  if (ai != bi)
    return false;
  auto ad = source_string(ag, an.strings[0]),
       bd = source_string(bg, bn.strings[0]);
  if (!ad || !bd)
    return ad.has_value() == bd.has_value();
  auto av = utf16(*ad, max_text_units), bv = utf16(*bd, max_text_units);
  if (!av || !bv)
    return {};
  return *av == *bv;
}
template <class T> bool ordered(SearchOperator op, T a, T b) {
  switch (op) {
  case SearchOperator::less_than:
    return a < b;
  case SearchOperator::less_equal:
    return a <= b;
  case SearchOperator::greater_equal:
    return a >= b;
  case SearchOperator::greater_than:
    return a > b;
  default:
    return false;
  }
}
void step(uint64_t &budget) {
  detail::require(budget > 0, "search matching resource limit");
  --budget;
}
bool contains(std::u16string_view s, std::u16string_view p, uint64_t budget) {
  // Native Find rejects both empty operands, including an empty needle.
  if (s.empty() || p.empty() || p.size() > s.size())
    return false;
  std::vector<size_t> failure(p.size());
  for (size_t i = 1, matched = 0; i < p.size(); ++i) {
    step(budget);
    while (matched && p[i] != p[matched]) {
      step(budget);
      matched = failure[matched - 1];
    }
    if (p[i] == p[matched])
      ++matched;
    failure[i] = matched;
  }
  for (size_t i = 0, matched = 0; i < s.size(); ++i) {
    step(budget);
    while (matched && s[i] != p[matched]) {
      step(budget);
      matched = failure[matched - 1];
    }
    if (s[i] == p[matched])
      ++matched;
    if (matched == p.size())
      return true;
  }
  return false;
}
bool wildcard(std::u16string_view s, std::u16string_view p, char16_t any,
              char16_t star, uint64_t budget) {
  size_t i = 0, j = 0, retry = 0, after_star = std::u16string_view::npos;
  while (i < s.size()) {
    step(budget);
    if (j < p.size() && p[j] == star) {
      after_star = ++j;
      retry = i;
    } else if (j < p.size() && (p[j] == any || p[j] == s[i])) {
      ++i;
      ++j;
    } else if (after_star != std::u16string_view::npos) {
      j = after_star;
      i = ++retry;
    } else
      return false;
  }
  while (j < p.size() && p[j] == star) {
    step(budget);
    ++j;
  }
  return j == p.size();
}
Status text_match(SearchOperator op, SearchValueView a, SearchValueView b,
                  const SearchValueOptions &options) {
  auto as = source_string(a.strings, a.value.string);
  auto bs = source_string(b.strings, b.value.string);
  bool equality =
      op == SearchOperator::equals || op == SearchOperator::not_equals;
  if (!equality && op != SearchOperator::contains &&
      op != SearchOperator::wildcard)
    return Status::no_match;
  const auto &flags = options.text;
  bool normalize =
      flags.ignore_case || flags.ignore_accents || flags.ignore_widths;
  if (equality && !normalize && (!as || !bs))
    return result((as.has_value() == bs.has_value()) ==
                  (op == SearchOperator::equals));
  auto av = utf16(as.value_or(""), options.max_text_units);
  auto bv = utf16(bs.value_or(""), options.max_text_units);
  if (!av || !bv)
    return Status::unsupported_value;
  if (normalize) {
    if (!options.transform || !as || !bs)
      return Status::text_context_required;
    av = options.transform(*av, flags);
    bv = options.transform(*bv, flags);
    if (!av || !bv)
      return Status::text_context_required;
    detail::require(av->size() <= options.max_text_units &&
                        bv->size() <= options.max_text_units,
                    "search transformed text resource limit");
  }
  auto trim = [](const std::u16string &s) {
    return std::u16string_view(s).substr(0, s.find(u'\0'));
  };
  auto left = trim(*av), right = trim(*bv);
  if (equality)
    return result((left == right) == (op == SearchOperator::equals));
  if (op == SearchOperator::contains)
    return result(contains(left, right, options.max_match_steps));
  return result(wildcard(left, right, flags.ignore_widths ? u'\uff1f' : u'?',
                         flags.ignore_widths ? u'\uff0a' : u'*',
                         options.max_match_steps));
}
} // namespace
bool search_storage_type_equal(const Value &a, const Value &b) noexcept {
  return a.tag <= 16 && b.tag <= 16 &&
         (a.tag == b.tag || (floating(a.tag) && floating(b.tag)));
}
SearchValueStatus search_value_match(SearchOperator op, SearchValueView a,
                                     SearchValueView b,
                                     const SearchValueOptions &options) {
  if (op < SearchOperator::same_type || op > SearchOperator::within_week)
    return Status::unsupported_operation;
  if (a.value.tag > 16 || b.value.tag > 16)
    return Status::unsupported_value;
  if ((a.value.tag == 3 && a.value.integer != 0 && a.value.integer != 1) ||
      (b.value.tag == 3 && b.value.integer != 0 && b.value.integer != 1))
    return Status::unsupported_value;
  if (!search_storage_type_equal(a.value, b.value))
    return Status::no_match;
  if (op == SearchOperator::same_type)
    return Status::match;
  auto tag = b.value.tag;
  if (tag == 4)
    return text_match(op, a, b, options);
  if (op == SearchOperator::equals || op == SearchOperator::not_equals) {
    bool equal = false;
    if (floating(tag))
      equal = a.value.number[0] == b.value.number[0];
    else if (tag == 0)
      equal = true;
    else if (tag == 2 || tag == 3 || tag == 5 || tag >= 14)
      equal = a.value.integer == b.value.integer;
    else if (tag == 9)
      equal = source_string(a.strings, a.value.string) ==
              source_string(b.strings, b.value.string);
    else if (tag == 8) {
      auto same = name_equal(a, b, options.max_text_units);
      if (!same)
        return Status::unsupported_value;
      equal = *same;
    } else {
      equal = true;
      for (unsigned i = 0, count = tag == 12 ? 3 : 2; i < count; ++i)
        equal = equal && a.value.number[i] == b.value.number[i];
    }
    return result(equal == (op == SearchOperator::equals));
  }
  if (floating(tag) || tag == 2 || tag >= 14) {
    if (op < SearchOperator::less_than || op > SearchOperator::greater_than)
      return Status::no_match;
    if (options.compare_numeric) {
      auto compared = options.compare_numeric(a, b);
      return compared ? result(ordered(op, *compared, 0))
                      : Status::numeric_context_required;
    }
    if (!options.exact_numeric_order)
      return Status::numeric_context_required;
    if (floating(tag))
      return result(ordered(op, a.value.number[0], b.value.number[0]));
    if (tag == 14 || tag == 16)
      return result(
          ordered(op, a.value.unsigned_integer(), b.value.unsigned_integer()));
    return result(ordered(op, a.value.integer, b.value.integer));
  }
  if (tag == 5) {
    if (op == SearchOperator::within_day || op == SearchOperator::within_week) {
      uint64_t width = op == SearchOperator::within_day ? 86400 : 604800;
      auto lower = std::bit_cast<int64_t>(
          static_cast<uint64_t>(b.value.integer) - width);
      return result(a.value.integer <= b.value.integer &&
                    a.value.integer > lower);
    }
    return result(ordered(op, a.value.integer, b.value.integer));
  }
  return Status::no_match;
}
} // namespace nwd
