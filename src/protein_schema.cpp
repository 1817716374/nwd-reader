#include "internal.hpp"
#include "expat/nwd_expat_prefix.h"
#define XML_STATIC 1
#include "expat/lib/expat.h"
#include <exception>
#include <limits>
#include <charconv>
#include <map>
#include <set>
#include <cmath>
#include <bit>
#include <unordered_map>
#include <unordered_set>

namespace nwd {
namespace {
std::optional<uint32_t> schema_length_unit(std::string_view name) noexcept {
  static constexpr std::pair<std::string_view, uint32_t> units[]{
      {"mm", 0x200e},          {"millimeter", 0x200e},  {"millimetre", 0x200e},
      {"millimeters", 0x200e}, {"millimetres", 0x200e}, {"cm", 0x200d},
      {"centimeter", 0x200d},  {"centimetre", 0x200d},  {"centimeters", 0x200d},
      {"centimetres", 0x200d}, {"dm", 0x200c},          {"decimeter", 0x200c},
      {"decimetre", 0x200c},   {"decimeters", 0x200c},  {"decimetres", 0x200c},
      {"m", 0x2001},           {"meter", 0x2001},       {"metre", 0x2001},
      {"meters", 0x2001},      {"metres", 0x2001},      {"km", 0x2009},
      {"kilometer", 0x2009},   {"kilometre", 0x2009},   {"kilometers", 0x2009},
      {"kilometres", 0x2009},  {"in", 0x2016},          {"inch", 0x2016},
      {"inches", 0x2016},      {"ft", 0x2017},          {"foot", 0x2017},
      {"feet", 0x2017},        {"yd", 0x201d},          {"yard", 0x201d},
      {"yards", 0x201d},       {"mi", 0x2021},          {"mile", 0x2021},
      {"miles", 0x2021}};
  for (const auto &[key, code] : units)
    if (name == key)
      return code;
  return std::nullopt;
}
} // namespace
ProteinSchemaPreparedFields
protein_schema_prepare_fields(const ProteinSchemaEntryFields &fields,
                              std::string_view effective_id) noexcept {
  ProteinSchemaPreparedFields out{fields.label.value_or(std::string_view{}),
                                  fields.declaration_flags};
  if ((out.flags & 1) && out.label.empty())
    out.label = effective_id;
  const auto kind = fields.kind;
  if (kind == "TextureURI")
    out.flags |= 0x204;
  else if (kind == "implementation")
    out.flags = 0x102;
  else if (kind == "ui")
    out.flags = 0x82;
  else if (kind == "Distance")
    out.flags |= 0x400;
  else if (kind == "Vector3")
    out.flags &= ~4u;
  else if ((kind == "String" && effective_id == "assettype") || kind == "type")
    out.flags = 2;
  const auto unit = fields.unit.value_or(std::string_view{});
  if (schema_length_unit(unit))
    out.flags |= 0x400;
  return out;
}

std::optional<uint32_t>
protein_schema_value_type(std::string_view name) noexcept {
  static constexpr std::pair<std::string_view, uint32_t> types[] = {
      {"Base", 2},       {"UID", 2},        {"String", 2},
      {"type", 2},       {"localtype", 2},  {"localname", 2},
      {"DateTime", 3},   {"Binary", 1},     {"Uuid", 11},
      {"URL", 4},        {"TextureURI", 4}, {"Boolean", 5},
      {"Integer", 6},    {"Float", 7},      {"Distance", 7},
      {"Choice", 10},    {"Color", 8},      {"texture", 9},
      {"Reference", 12}, {"ui", 2},         {"implementation", 2},
      {"Vector3", 7}};
  for (const auto &[key, value] : types)
    if (name == key)
      return value;
  return std::nullopt;
}
uint32_t protein_schema_flag_conflicts(uint32_t type, uint32_t flags) noexcept {
  uint32_t errors = flags > 0xfff ? 1 : 0;
  if ((flags & 0xa) == 0xa)
    errors |= 2;
  const auto connections = flags & 0x60;
  if (connections == 0x60 || ((type == 9 || type == 12) && !connections))
    errors |= 32;
  if ((flags & 4) && !(type <= 10 && ((0x5d4u >> type) & 1)))
    errors |= 4;
  if ((flags & 0x400) && type != 7)
    errors |= 8;
  if ((flags & 0x380) && type != 4)
    errors |= 16;
  return errors;
}
namespace {
using Kind = ProteinSchemaNodeKind;
struct Parser {
  XML_Parser xml = XML_ParserCreate(nullptr);
  ProteinSchemaXmlOptions limits;
  std::vector<ProteinSchemaXmlNode> nodes;
  std::vector<Id> stack, last_child, entries;
  Id root = none, last_top = none;
  uint64_t text_bytes = 0, attributes = 0;
  std::exception_ptr error;
  explicit Parser(ProteinSchemaXmlOptions opts) : limits(opts) {
    if (!xml)
      throw std::bad_alloc();
  }
  ~Parser() { XML_ParserFree(xml); }
  Parser(const Parser &) = delete;
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
  void account(size_t bytes) {
    detail::require(bytes <= limits.max_text_bytes - text_bytes,
                    "Protein Schema XML text budget");
    text_bytes += bytes;
  }
  std::string copy(std::string_view text) {
    account(text.size());
    return std::string(text);
  }
  Id add(Kind kind, std::string_view name, std::string_view value) {
    detail::require(nodes.size() < limits.max_nodes && nodes.size() < none,
                    "Protein Schema XML node budget");
    const Id id = static_cast<Id>(nodes.size());
    ProteinSchemaXmlNode node;
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
    auto &p = *static_cast<Parser *>(arg);
    p.guard([&] {
      detail::require(p.stack.size() < p.limits.max_depth,
                      "Protein Schema XML depth budget");
      if (p.stack.empty())
        detail::require(p.root == none &&
                            std::string_view(name) == "AssetSchema",
                        "Protein Schema XML requires AssetSchema root");
      const Id id = p.add(Kind::element, name, {});
      if (p.stack.empty())
        p.root = id;
      else if (p.stack.back() == p.root)
        p.entries.push_back(id);
      for (size_t i = 0; attrs[i]; i += 2) {
        detail::require(p.attributes < p.limits.max_attributes,
                        "Protein Schema XML attribute budget");
        ++p.attributes;
        p.nodes[id].attributes.push_back(
            {p.copy(attrs[i]), p.copy(attrs[i + 1])});
      }
      p.stack.push_back(id);
      p.last_child.push_back(none);
    });
  }
  static void XMLCALL end(void *arg, const XML_Char *) noexcept {
    auto &p = *static_cast<Parser *>(arg);
    p.guard([&] {
      p.stack.pop_back();
      p.last_child.pop_back();
    });
  }
  static void XMLCALL text(void *arg, const XML_Char *value,
                           int size) noexcept {
    auto &p = *static_cast<Parser *>(arg);
    p.guard([&] {
      if (!size)
        return;
      const Id previous = p.stack.empty() ? p.last_top : p.last_child.back();
      const auto v = std::string_view(value, static_cast<size_t>(size));
      if (previous != none && p.nodes[previous].kind == Kind::text) {
        p.account(v.size());
        p.nodes[previous].value.append(v);
      } else
        p.add(Kind::text, {}, v);
    });
  }
  static void XMLCALL comment(void *arg, const XML_Char *value) noexcept {
    auto &p = *static_cast<Parser *>(arg);
    p.guard([&] { p.add(Kind::comment, {}, value); });
  }
  static void XMLCALL instruction(void *arg, const XML_Char *name,
                                  const XML_Char *value) noexcept {
    auto &p = *static_cast<Parser *>(arg);
    p.guard([&] { p.add(Kind::instruction, name, value); });
  }
  static void XMLCALL doctype(void *arg, const XML_Char *, const XML_Char *,
                              const XML_Char *, int) noexcept {
    auto &p = *static_cast<Parser *>(arg);
    p.guard([] { throw Error("Protein Schema XML DTD is unsupported"); });
  }
  void parse(std::span<const uint8_t> bytes) {
    detail::require(bytes.size() <= limits.max_bytes,
                    "Protein Schema XML input budget");
    XML_SetUserData(xml, this);
    XML_SetElementHandler(xml, start, end);
    XML_SetCharacterDataHandler(xml, text);
    XML_SetCommentHandler(xml, comment);
    XML_SetProcessingInstructionHandler(xml, instruction);
    XML_SetStartDoctypeDeclHandler(xml, doctype);
    // Bounded calls avoid int truncation and Expat's whole-input buffer copy.
    size_t at = 0;
    do {
      const auto n = std::min<size_t>(bytes.size() - at, 65536);
      const char *data =
          n ? reinterpret_cast<const char *>(bytes.data() + at) : "";
      const auto status =
          XML_Parse(xml, data, static_cast<int>(n), at + n == bytes.size());
      if (error)
        std::rethrow_exception(error);
      if (status != XML_STATUS_OK)
        throw Error("Protein Schema XML at byte " +
                    std::to_string(XML_GetCurrentByteIndex(xml)) + ": " +
                    XML_ErrorString(XML_GetErrorCode(xml)));
      at += n;
    } while (at < bytes.size());
    detail::require(root != none && stack.empty(),
                    "Protein Schema XML root absent");
  }
};
} // namespace
struct ProteinSchemaDocument::Impl {
  std::vector<ProteinSchemaXmlNode> nodes;
  std::vector<Id> entries;
  Id root;
  Impl(std::span<const uint8_t> bytes, ProteinSchemaXmlOptions options) {
    Parser p(options);
    p.parse(bytes);
    nodes = std::move(p.nodes);
    entries = std::move(p.entries);
    root = p.root;
  }
};
ProteinSchemaDocument::ProteinSchemaDocument(std::span<const uint8_t> bytes,
                                             ProteinSchemaXmlOptions options)
    : impl(std::make_shared<Impl>(bytes, options)) {}
std::span<const ProteinSchemaXmlNode> ProteinSchemaDocument::nodes() const {
  return impl->nodes;
}
Id ProteinSchemaDocument::root() const { return impl->root; }
std::span<const Id> ProteinSchemaDocument::entries() const {
  return impl->entries;
}
ProteinSchemaEntryFields ProteinSchemaDocument::entry_fields(Id id) const {
  detail::require(id < impl->nodes.size() &&
                      impl->nodes[id].parent == impl->root &&
                      impl->nodes[id].kind == Kind::element,
                  "Protein Schema entry is not an immediate declaration");
  const auto &node = impl->nodes[id];
  ProteinSchemaEntryFields out;
  out.kind = node.name;
  auto flag = [&](uint32_t mask, bool value) {
    if (value)
      out.declaration_flags |= mask;
    else
      out.declaration_flags &= ~mask;
  };
  for (const auto &a : node.attributes) {
    const std::string_view value = a.value;
    if (a.name == "id")
      out.id = value;
    else if (a.name == "val")
      out.value = value;
    else if (a.name == "type")
      out.type = value;
    else if (a.name == "label")
      out.label = value;
    else if (a.name == "unit")
      out.unit = value;
    else if (a.name == "alias")
      out.alias = value;
    else if (a.name == "file")
      out.file = value;
    else if (a.name == "valR")
      out.rgba[0] = value;
    else if (a.name == "valG")
      out.rgba[1] = value;
    else if (a.name == "valB")
      out.rgba[2] = value;
    else if (a.name == "valA")
      out.rgba[3] = value;
    else if (a.name == "valX")
      out.xyz[0] = value;
    else if (a.name == "valY")
      out.xyz[1] = value;
    else if (a.name == "valZ")
      out.xyz[2] = value;
    else if (a.name == "public")
      flag(1, value != "false");
    else if (a.name == "readonly")
      flag(2, value == "true");
    else if (a.name == "allowmultiplevalues")
      flag(4, value == "true");
    else if (a.name == "definitionIteratorData")
      flag(8, value == "true");
    else if (a.name == "local")
      flag(16, value != "false");
    else if (a.name == "allowconnectedassets") {
      flag(32, value == "single");
      flag(64, value == "multiple");
    } else if (a.name == "metadata")
      flag(2048, value == "true");
  }
  return out;
}

namespace {
using NumberStatus = ProteinSchemaNumberStatus;
ProteinSchemaNumber default_number(double value) {
  return {{}, value, NumberStatus::defaulted, 0};
}
bool decimal_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}
bool digit(char c) { return c >= '0' && c <= '9'; }
ProteinSchemaNumber number(std::string text, bool integer = false) {
  ProteinSchemaNumber out;
  out.text = std::move(text);
  const std::string_view s = out.text;
  size_t start = 0;
  while (start < s.size() && decimal_space(s[start]))
    ++start;
  if (start == s.size()) {
    out.status = NumberStatus::empty;
    return out;
  }
  size_t end = start;
  if (s[end] == '+' || s[end] == '-')
    ++end;
  const auto digits_start = end;
  while (end < s.size() && digit(s[end]))
    ++end;
  size_t digits = end - digits_start;
  if (!integer && end < s.size() && s[end] == '.') {
    const auto after_dot = ++end;
    while (end < s.size() && digit(s[end]))
      ++end;
    digits += end - after_dot;
  }
  if (!digits) {
    out.status = NumberStatus::invalid;
    return out;
  }
  if (!integer && end < s.size() && (s[end] == 'e' || s[end] == 'E')) {
    ++end;
    if (end < s.size() && (s[end] == '+' || s[end] == '-'))
      ++end;
    const auto exponent_start = end;
    while (end < s.size() && digit(s[end]))
      ++end;
    if (end == exponent_start) {
      out.status = NumberStatus::invalid;
      return out;
    }
  }
  const char *first = s.data() + start + (s[start] == '+');
  const char *last = s.data() + end;
  std::from_chars_result parsed;
  double value = 0;
  if (integer) {
    int32_t v = 0;
    parsed = std::from_chars(first, last, v, 10);
    value = v;
  } else
    parsed = std::from_chars(first, last, value, std::chars_format::general);
  out.consumed = end;
  if (parsed.ec != std::errc{} || parsed.ptr != last) {
    out.status = parsed.ec == std::errc::result_out_of_range
                     ? NumberStatus::out_of_range
                     : NumberStatus::invalid;
    return out;
  }
  out.value = value;
  out.status = end == s.size() ? NumberStatus::exact : NumberStatus::prefix;
  return out;
}
struct ValueBudget {
  uint64_t remaining;
  void use(size_t count) {
    detail::require(count <= remaining, "Protein Schema value budget");
    remaining -= count;
  }
};
template <bool Materialize = true, bool FirstOnly = false, class F>
void tokens(std::string_view text, ValueBudget &budget, F &&emit) {
  std::string token;
  bool escaped = false;
  [[maybe_unused]] bool first = true;
  for (char c : text) {
    if (!escaped && (c == ',' || c == '|')) {
      budget.use(1);
      emit(std::move(token));
      token.clear();
      first = false;
    } else if (!escaped && c == '\\')
      escaped = true;
    else {
      if constexpr (Materialize) {
        if (!FirstOnly || first)
          token.push_back(c);
      }
      escaped = false;
    }
  }
  // Native splitter preserves empty/trailing tokens and drops a dangling '\'.
  budget.use(1);
  emit(std::move(token));
}
} // namespace

ProteinSchemaDefaults
ProteinSchemaDocument::defaults(Id id,
                                ProteinSchemaValueOptions options) const {
  const auto fields = entry_fields(id);
  ProteinSchemaDefaults out;
  out.effective_id = fields.id.value_or(std::string_view{});
  if (fields.kind == "localname" || fields.kind == "localtype")
    out.effective_id = fields.kind;
  else if (fields.kind == "type")
    out.effective_id = "assettype";
  ValueBudget budget{options.max_values};
  if (fields.kind == "ui" || fields.kind == "implementation") {
    budget.use(1);
    out.tokens.emplace_back(fields.value.value_or(std::string_view{}));
  } else if (fields.value)
    tokens(*fields.value, budget,
           [&](std::string value) { out.tokens.push_back(std::move(value)); });
  if (fields.kind == "Float" || fields.kind == "Distance" ||
      fields.kind == "Integer")
    out.scalar = out.tokens.empty()
                     ? default_number(0)
                     : number(out.tokens.front(), fields.kind == "Integer");
  else if (fields.kind == "Boolean")
    out.boolean_value = !out.tokens.empty() && out.tokens.front() == "true";
  else if (fields.kind == "Reference")
    out.texture_reference = fields.type == "texture";
  else if (fields.kind == "Color" || fields.kind == "Vector3") {
    auto components = fields.rgba;
    for (size_t c = 0; c < 3; ++c)
      if (fields.xyz[c])
        components[c] = fields.xyz[c];
    const bool color = fields.kind == "Color";
    const size_t count = color ? 4 : 3;
    size_t length = color ? 0 : 1;
    for (size_t c = 0; c < count; ++c) {
      if (components[c])
        tokens(*components[c], budget, [&](std::string token) {
          // Vector3 truncates each input component to one number. Still scan
          // and charge every token, without allocating discarded number arrays.
          if (color || out.components[c].empty())
            out.components[c].push_back(number(std::move(token)));
        });
      if (color)
        length = std::max(length, out.components[c].size());
    }
    for (size_t c = 0; c < count; ++c) {
      const auto padding = length - out.components[c].size();
      budget.use(padding);
      out.components[c].resize(length, default_number(c == 3 ? 1 : 0));
    }
  }
  return out;
}

ProteinSchemaTextDefaults
ProteinSchemaDocument::text_defaults(Id entry,
                                     ProteinSchemaValueOptions options) const {
  const auto fields = entry_fields(entry);
  const auto kind = fields.kind;
  ProteinSchemaTextDefaults out;
  const bool raw = kind == "ui" || kind == "implementation";
  const bool special =
      kind == "type" || kind == "localtype" || kind == "localname";
  if (kind == "String" || special)
    out.value_type = 2;
  else if (kind == "DateTime")
    out.value_type = 3;
  else if (kind == "Uuid")
    out.value_type = 11;
  else if (kind == "URL" || kind == "TextureURI" || raw)
    out.value_type = 4;
  else
    return out;
  const auto effective_id = kind == "type" ? std::string_view("assettype")
                            : special      ? kind
                                      : fields.id.value_or(std::string_view{});
  out.flags = protein_schema_prepare_fields(fields, effective_id).flags;
  out.multiple = (out.flags & 4) && kind != "Uuid" && !special;
  // Special string factories inspect the original val string, before using
  // the split token vector. A nonempty escape-only val can assign empty text.
  const bool rejected =
      (kind == "DateTime" && out.multiple) ||
      (special && fields.value.value_or(std::string_view{}).empty());
  ValueBudget budget{options.max_values};
  // Even when a factory selects only the first token, preprocessing visits
  // every input token. Retain only the selected strings, avoiding a second
  // full-sized vector for large scalar declarations.
  auto emit = [&](std::string value) {
    if (!rejected && (out.multiple || out.values.empty()))
      out.values.push_back(std::move(value));
  };
  if (raw) {
    budget.use(1);
    emit(std::string(fields.value.value_or(std::string_view{})));
  } else if (fields.value) {
    tokens(*fields.value, budget, emit);
  }
  if (rejected) {
    out.status = ProteinSchemaTextStatus::rejected;
    out.values.clear();
    return out;
  }
  out.uses_type_default = !out.multiple && out.values.empty();
  out.status = ProteinSchemaTextStatus::ready;
  return out;
}

ProteinSchemaConstructedDefaults ProteinSchemaDocument::constructed_defaults(
    Id entry, ProteinSchemaValueOptions options) const {
  const auto fields = entry_fields(entry);
  ProteinSchemaConstructedDefaults out;
  if (fields.kind == "Binary")
    out.kind = ProteinSchemaConstructedKind::empty_binary;
  else if (fields.kind == "Reference")
    out.kind = fields.type == "texture"
                   ? ProteinSchemaConstructedKind::empty_texture_link
                   : ProteinSchemaConstructedKind::empty_reference_array;
  else
    return out;
  out.flags = protein_schema_prepare_fields(
                  fields, fields.id.value_or(std::string_view{}))
                  .flags;
  ValueBudget budget{options.max_values};
  if (fields.value)
    tokens<false>(*fields.value, budget, [](std::string) {});
  return out;
}

ProteinSchemaIdentityAssignment ProteinSchemaDocument::identity_assignment(
    Id entry, std::string_view current_value,
    ProteinSchemaValueOptions options) const {
  const auto fields = entry_fields(entry);
  ProteinSchemaIdentityAssignment out;
  if (fields.kind == "UID")
    out.kind = ProteinSchemaIdentityKind::uid;
  else if (fields.kind == "Base")
    out.kind = ProteinSchemaIdentityKind::base_schema;
  else
    return out;
  ValueBudget budget{options.max_values};
  uint64_t count = 0;
  std::string value;
  if (fields.value)
    tokens<true, true>(*fields.value, budget, [&](std::string token) {
      if (!count++)
        value = std::move(token);
    });
  if (count != 1 || (out.kind == ProteinSchemaIdentityKind::uid
                         ? fields.value.value_or(std::string_view{}).empty()
                         : value.empty())) {
    out.status = ProteinSchemaIdentityStatus::invalid_value;
    return out;
  }
  if (!current_value.empty()) {
    out.status = ProteinSchemaIdentityStatus::redefined;
    return out;
  }
  out.status = ProteinSchemaIdentityStatus::ready;
  out.value = std::move(value);
  if (out.kind == ProteinSchemaIdentityKind::base_schema)
    out.flags = 8;
  return out;
}

namespace {
template <class F>
void visit_schema_descendants(std::span<const ProteinSchemaXmlNode> nodes,
                              Id entry, F &&visit) {
  // Arena order is preorder. Climb through parents only after a subtree ends;
  // each edge is visited at most twice, with no recursive C++ stack growth.
  Id id = nodes[entry].first_child;
  while (id != none) {
    if (nodes[id].kind == Kind::element)
      visit(id);
    if (nodes[id].first_child != none) {
      id = nodes[id].first_child;
      continue;
    }
    while (id != entry && nodes[id].next_sibling == none)
      id = nodes[id].parent;
    if (id == entry)
      break;
    id = nodes[id].next_sibling;
  }
}
template <class F>
void visit_constraint_nodes(std::span<const ProteinSchemaXmlNode> nodes,
                            Id entry, std::string_view kind, F &&visit) {
  visit_schema_descendants(nodes, entry, [&](Id id) {
    if (!(kind == "Choice" && nodes[id].name == "ChoiceValue"))
      visit(id);
  });
}
} // namespace

struct ProteinSchemaChoiceIndex::Impl {
  ProteinSchemaDocument document;
  Id declaration = none;
  std::vector<ProteinSchemaChoiceEntry> entries;
  std::map<std::string_view, Id, std::less<>> ids;
  bool complete = true;
  explicit Impl(const ProteinSchemaDocument &doc) : document(doc) {}
};
ProteinSchemaChoiceIndex::ProteinSchemaChoiceIndex(
    const ProteinSchemaDocument &doc, Id entry,
    ProteinSchemaValueOptions options) {
  detail::require(doc.entry_fields(entry).kind == "Choice",
                  "Protein Schema choice index requires a Choice declaration");
  auto result = std::make_shared<Impl>(doc);
  result->declaration = entry;
  std::map<int32_t, Id> values;
  ValueBudget budget{options.max_values};
  using Status = ProteinSchemaChoiceStatus;
  visit_schema_descendants(doc.nodes(), entry, [&](Id node) {
    if (doc.nodes()[node].name != "ChoiceValue")
      return;
    budget.use(1);
    ProteinSchemaChoiceEntry item;
    item.source = node;
    for (const auto &a : doc.nodes()[node].attributes) {
      if (a.name == "id")
        item.id = a.value;
      else if (a.name == "label")
        item.label = a.value;
      else if (a.name == "val")
        item.raw_value = a.value;
    }
    item.value =
        number(std::string(item.raw_value.value_or(std::string_view{})), true);
    // The native handler dereferences attribute lookup results without testing
    // for end(). Do not invent default values for missing required attributes.
    if (!item.id || !item.label || !item.raw_value) {
      item.status = Status::unresolved_attributes;
      result->complete = false;
    } else if (item.id->empty()) {
      item.status = Status::empty_id;
    } else if (!item.value.value) {
      item.status = Status::unresolved_value;
      result->complete = false;
    } else if (*item.value.value < 0) {
      item.status = Status::negative_value;
    } else if (auto it = result->ids.find(*item.id); it != result->ids.end()) {
      item.status = Status::duplicate_id;
      item.conflict = it->second;
    } else if (auto it = values.find(static_cast<int32_t>(*item.value.value));
               it != values.end()) {
      item.status = Status::duplicate_value;
      item.conflict = it->second;
    } else if (!result->complete) {
      item.status = Status::unresolved_previous;
    } else {
      item.status = Status::accepted;
      const auto position = static_cast<Id>(result->entries.size());
      result->ids.emplace(*item.id, position);
      values.emplace(static_cast<int32_t>(*item.value.value), position);
    }
    result->entries.push_back(std::move(item));
  });
  impl = std::move(result);
}
std::span<const ProteinSchemaChoiceEntry>
ProteinSchemaChoiceIndex::entries() const noexcept {
  return impl->entries;
}
bool ProteinSchemaChoiceIndex::complete() const noexcept {
  return impl->complete;
}
ProteinSchemaChoiceLookup
ProteinSchemaChoiceIndex::find_id(std::string_view id) const {
  if (auto it = impl->ids.find(id); it != impl->ids.end())
    return {ProteinSchemaChoiceLookupStatus::found, it->second};
  return {impl->complete ? ProteinSchemaChoiceLookupStatus::missing
                         : ProteinSchemaChoiceLookupStatus::unresolved,
          none};
}

ProteinSchemaChoiceDefaults
ProteinSchemaChoiceIndex::defaults(ProteinSchemaValueOptions options) const {
  using Status = ProteinSchemaChoiceDefaultStatus;
  using TokenStatus = ProteinSchemaChoiceTokenStatus;
  ProteinSchemaChoiceDefaults out;
  const auto fields = impl->document.entry_fields(impl->declaration);
  out.multiple = (fields.declaration_flags & 4) != 0;
  ValueBudget budget{options.max_values};
  if (fields.value)
    tokens(*fields.value, budget, [&](std::string text) {
      out.tokens.push_back({std::move(text), TokenStatus::ignored, none});
    });
  if (out.tokens.empty())
    return out;
  out.status = impl->complete ? Status::ready : Status::unresolved;
  const size_t count = out.multiple ? out.tokens.size() : 1;
  for (size_t i = 0; i < count; ++i) {
    auto &token = out.tokens[i];
    const auto found = find_id(token.text);
    if (found.status == ProteinSchemaChoiceLookupStatus::found) {
      token.status = TokenStatus::matched;
      token.entry = found.entry;
      if (out.status == Status::ready)
        out.values.push_back(
            static_cast<int32_t>(*impl->entries[found.entry].value.value));
    } else if (found.status == ProteinSchemaChoiceLookupStatus::missing) {
      token.status = TokenStatus::missing;
      if (i == 0) {
        out.status = Status::missing_first;
        break;
      }
    } else {
      token.status = TokenStatus::unresolved;
      out.status = Status::unresolved;
      if (i == 0)
        break;
    }
  }
  if (out.status != Status::ready)
    out.values.clear();
  return out;
}

std::vector<Id> ProteinSchemaDocument::constraint_nodes(Id entry) const {
  const auto fields = entry_fields(entry);
  std::vector<Id> out;
  visit_constraint_nodes(impl->nodes, entry, fields.kind,
                         [&](Id id) { out.push_back(id); });
  return out;
}

ProteinSchemaResolvedConstraint
ProteinSchemaDocument::resolved_constraint(Id entry) const {
  const auto fields = entry_fields(entry);
  ProteinSchemaResolvedConstraint out;
  visit_constraint_nodes(impl->nodes, entry, fields.kind,
                         [&](Id id) { out.source = id; });
  if (out.source == none)
    return out;
  out.fields = std::make_shared<const ProteinSchemaConstraintFields>(
      constraint_fields(out.source));
  const auto &raw = *out.fields;
  using Status = ProteinSchemaConstraintStatus;
  if (raw.percentage) {
    out.status = Status::ready;
    out.range = ProteinSchemaNumericRange{0, 1, 1, true, true, true};
    return out;
  }
  if (raw.minimum && raw.maximum && raw.minimum->value && raw.maximum->value) {
    const auto low = *raw.minimum->value, high = *raw.maximum->value;
    if (low > high ||
        (low == high && (!raw.minimum_inclusive || !raw.maximum_inclusive))) {
      out.status = Status::discarded;
      return out;
    }
  }
  if ((raw.minimum && !raw.minimum->value) ||
      (raw.maximum && !raw.maximum->value) || !raw.increment.value) {
    out.status = Status::unresolved;
    return out;
  }
  ProteinSchemaNumericRange range;
  if (raw.minimum)
    range.minimum = raw.minimum->value;
  if (raw.maximum)
    range.maximum = raw.maximum->value;
  range.increment = *raw.increment.value;
  range.minimum_inclusive = raw.minimum_inclusive;
  range.maximum_inclusive = raw.maximum_inclusive;
  out.status = Status::ready;
  out.range = range;
  return out;
}

ProteinSchemaConstraintFields
ProteinSchemaDocument::constraint_fields(Id id) const {
  const auto &nodes = impl->nodes;
  detail::require(id < nodes.size() && nodes[id].kind == Kind::element &&
                      nodes[id].parent != none &&
                      nodes[id].parent != impl->root,
                  "Protein Schema constraint is not a nested element");
  Id entry = id;
  while (nodes[entry].parent != impl->root)
    entry = nodes[entry].parent;
  detail::require(
      !(nodes[entry].name == "Choice" && nodes[id].name == "ChoiceValue"),
      "Protein Schema ChoiceValue is not a numeric constraint");
  ProteinSchemaConstraintFields out;
  out.increment = default_number(1);
  for (const auto &a : nodes[id].attributes) {
    if (a.name == "minimum")
      out.minimum = number(a.value);
    else if (a.name == "maximum")
      out.maximum = number(a.value);
    else if (a.name == "incrementsize")
      out.increment = number(a.value);
    else if (a.name == "minimumisinclusive")
      out.minimum_inclusive = a.value == "true";
    else if (a.name == "maximumisinclusive")
      out.maximum_inclusive = a.value == "true";
    else if (a.name == "percentage")
      out.percentage = a.value == "true";
  }
  return out;
}

ProteinSchemaNumericDefaults ProteinSchemaDocument::numeric_defaults(
    Id entry, ProteinSchemaValueOptions options) const {
  const auto fields = entry_fields(entry);
  ProteinSchemaNumericDefaults out;
  const bool integer = fields.kind == "Integer";
  const bool real = fields.kind == "Float" || fields.kind == "Distance";
  const bool color = fields.kind == "Color", vector = fields.kind == "Vector3";
  if (!integer && !real && !color && !vector)
    return out;
  out.value_type = integer ? 6 : color ? 8 : 7;
  out.components = color ? 4 : vector ? 3 : 1;
  out.multiple = !vector && (fields.declaration_flags & 4);
  if (real && !out.multiple && fields.unit && !fields.unit->empty()) {
    out.unit = *fields.unit;
    out.unit_code = schema_length_unit(out.unit);
  }
  out.status = ProteinSchemaNumericStatus::ready;
  auto prepared = defaults(entry, options);
  bool uncertain = false;
  if (integer || real) {
    out.constraint = resolved_constraint(entry);
    auto bounds = out.constraint.range;
    if (out.constraint.status == ProteinSchemaConstraintStatus::unresolved) {
      const auto &raw = *out.constraint.fields;
      if ((!raw.minimum || raw.minimum->value) &&
          (!raw.maximum || raw.maximum->value)) {
        // Only the increment is unknown. Allocation and value acceptance do
        // not inspect it, so the known bounds still determine the value gate.
        ProteinSchemaNumericRange known;
        if (raw.minimum)
          known.minimum = raw.minimum->value;
        if (raw.maximum)
          known.maximum = raw.maximum->value;
        known.minimum_inclusive = raw.minimum_inclusive;
        known.maximum_inclusive = raw.maximum_inclusive;
        bounds = known;
      } else
        uncertain = true;
    }
    const auto first = prepared.scalar->value;
    if (bounds) {
      if (first) {
        if ((bounds->minimum &&
             (*first < *bounds->minimum ||
              (*first == *bounds->minimum && !bounds->minimum_inclusive))) ||
            (bounds->maximum &&
             (*first > *bounds->maximum ||
              (*first == *bounds->maximum && !bounds->maximum_inclusive))))
          out.rejections |= 1;
      } else
        uncertain = true;
      if (integer) {
        const double low = bounds->minimum.value_or(0),
                     high = bounds->maximum.value_or(0);
        if (std::floor(low) != low || std::floor(high) != high)
          out.rejections |= 2;
      }
    }
  } else if (color && !prepared.components[0].empty()) {
    for (size_t c = 0; c < 4; ++c) {
      const auto first = prepared.components[c][0].value;
      if (!first)
        uncertain = true;
      else if (*first < 0 || *first > (c == 3 ? 1 : 255))
        out.rejections |= 4;
    }
  }
  if (out.rejections) {
    out.status = ProteinSchemaNumericStatus::rejected;
    return out;
  }
  if (integer || real) {
    if (!out.multiple) {
      detail::require(options.max_values >= 1, "Protein Schema value budget");
      out.candidates.push_back(std::move(*prepared.scalar));
    } else {
      out.candidates.reserve(prepared.tokens.size());
      for (size_t i = 0; i < prepared.tokens.size(); ++i)
        out.candidates.push_back(
            i == 0 ? std::move(*prepared.scalar)
                   : number(std::move(prepared.tokens[i]), integer));
    }
  } else {
    const auto rows = out.multiple ? prepared.components[0].size() : 1;
    if (color && prepared.components[0].empty() && !out.multiple)
      detail::require(prepared.tokens.size() <= options.max_values &&
                          options.max_values - prepared.tokens.size() >= 4,
                      "Protein Schema value budget");
    out.candidates.reserve(rows * out.components);
    for (size_t i = 0; i < rows; ++i)
      for (size_t c = 0; c < out.components; ++c)
        out.candidates.push_back(prepared.components[c].empty()
                                     ? default_number(c == 3 ? 1 : 0)
                                     : std::move(prepared.components[c][i]));
  }
  for (const auto &candidate : out.candidates)
    uncertain |= !candidate.value;
  if (uncertain)
    out.status = ProteinSchemaNumericStatus::unresolved;
  return out;
}
struct ProteinSchemaAliasIndex::Impl {
  struct Local {
    std::shared_ptr<const ProteinSchemaDocument> document;
    std::vector<ProteinSchemaAliasEntry> entries;
    std::vector<ProteinSchemaNamedAlias> accepted;
    std::map<std::string_view, size_t, std::less<>> accepted_lookup;
    // Conflict pointers retain their original registration scope, even if a
    // later binding replaces the parent used for lookup.
    std::vector<std::pair<Id, std::shared_ptr<const Impl>>>
        registration_parents;
    uint32_t registration_depth = 0;
    std::vector<std::shared_ptr<const Impl>> imported_sources;
    explicit Local(const ProteinSchemaDocument &doc)
        : document(std::make_shared<const ProteinSchemaDocument>(doc)) {}
  };
  std::shared_ptr<const Local> local;
  std::shared_ptr<const Impl> parent;
  std::map<std::string_view, ProteinSchemaAliasBinding, std::less<>> visible;
  uint32_t depth = 1;
  void rebuild_visible(const ProteinSchemaAliasOptions &options) {
    const uint32_t prior_depth =
        std::max(parent ? parent->depth : 0, local->registration_depth);
    detail::require(prior_depth < options.max_scope_depth,
                    "Protein Schema alias scope depth limit");
    depth = prior_depth + 1;
    visible.clear();
    if (parent) {
      detail::require(parent->visible.size() <= options.max_visible_aliases,
                      "Protein Schema visible alias limit");
      visible = parent->visible;
    }
    for (const auto &[name, binding] : local->accepted) {
      if (!visible.contains(name))
        detail::require(visible.size() < options.max_visible_aliases,
                        "Protein Schema visible alias limit");
      visible.insert_or_assign(name, binding);
    }
  }
  Impl(const ProteinSchemaDocument &doc, std::shared_ptr<const Impl> p,
       ProteinSchemaAliasOptions options,
       std::span<const ProteinSchemaAliasParentChange> changes)
      : parent(std::move(p)) {
    detail::require(changes.size() <= options.max_parent_changes,
                    "Protein Schema alias parent-change limit");
    auto state = std::make_shared<Local>(doc);
    local = state;
    auto retain_parent = [&](Id before) {
      state->registration_parents.emplace_back(before, parent);
      if (parent) {
        state->registration_depth =
            std::max(state->registration_depth, parent->depth);
      }
      rebuild_visible(options);
    };
    retain_parent(0);
    size_t next_change = 0;
    std::map<std::string_view, ProteinSchemaAliasBinding, std::less<>> targets;
    for (Id source : doc.entries()) {
      if (next_change < changes.size() &&
          changes[next_change].before_entry == source) {
        const auto index = changes[next_change++].parent;
        parent = index ? index->impl : nullptr;
        retain_parent(source);
      }
      const auto fields = doc.entry_fields(source);
      if (fields.kind != "PropertyAlias")
        continue;
      detail::require(state->entries.size() < options.max_aliases,
                      "Protein Schema local alias limit");
      ProteinSchemaAliasEntry entry;
      entry.document = state->document.get();
      entry.source = source;
      entry.name = fields.alias;
      entry.target = fields.id;
      if (fields.alias.value_or(std::string_view{}).empty())
        entry.status = ProteinSchemaAliasStatus::empty_name;
      else if (fields.id.value_or(std::string_view{}).empty())
        entry.status = ProteinSchemaAliasStatus::empty_target;
      else if (auto i = visible.find(*fields.alias); i != visible.end()) {
        entry.status = ProteinSchemaAliasStatus::duplicate_name;
        entry.conflict = i->second;
      } else if (auto i = targets.find(*fields.id); i != targets.end()) {
        entry.status = ProteinSchemaAliasStatus::duplicate_local_target;
        entry.conflict = i->second;
      } else {
        detail::require(visible.size() < options.max_visible_aliases,
                        "Protein Schema visible alias limit");
        entry.status = ProteinSchemaAliasStatus::accepted;
        ProteinSchemaAliasBinding binding{state->document.get(), source,
                                          *fields.id};
        visible.emplace(*fields.alias, binding);
        targets.emplace(*fields.id, binding);
        state->accepted_lookup.emplace(*fields.alias, state->accepted.size());
        state->accepted.push_back({*fields.alias, binding});
      }
      state->entries.push_back(entry);
    }
    detail::require(next_change == changes.size(),
                    "Protein Schema alias parent-change order or source");
  }
  Impl(std::shared_ptr<const Local> state, std::shared_ptr<const Impl> p,
       ProteinSchemaAliasOptions options)
      : local(std::move(state)), parent(std::move(p)) {
    detail::require(local->entries.size() <= options.max_aliases,
                    "Protein Schema local alias limit");
    rebuild_visible(options);
  }
};
ProteinSchemaAliasIndex::ProteinSchemaAliasIndex(
    const ProteinSchemaDocument &doc, const ProteinSchemaAliasIndex *parent,
    ProteinSchemaAliasOptions options,
    std::span<const ProteinSchemaAliasParentChange> changes)
    : impl(std::make_shared<Impl>(doc, parent ? parent->impl : nullptr, options,
                                  changes)) {}
ProteinSchemaAliasIndex::ProteinSchemaAliasIndex(std::shared_ptr<const Impl> p)
    : impl(std::move(p)) {}
const ProteinSchemaDocument &
ProteinSchemaAliasIndex::document() const noexcept {
  return *impl->local->document;
}
std::span<const ProteinSchemaAliasEntry>
ProteinSchemaAliasIndex::entries() const noexcept {
  return impl->local->entries;
}
std::optional<ProteinSchemaAliasBinding>
ProteinSchemaAliasIndex::find(std::string_view name) const {
  const auto i = impl->visible.find(name);
  return i == impl->visible.end() ? std::optional<ProteinSchemaAliasBinding>{}
                                  : i->second;
}
std::span<const ProteinSchemaNamedAlias>
ProteinSchemaAliasIndex::local_aliases() const noexcept {
  return impl->local->accepted;
}
std::optional<ProteinSchemaAliasBinding>
ProteinSchemaAliasIndex::find_local(std::string_view name) const {
  const auto &local = *impl->local;
  const auto i = local.accepted_lookup.find(name);
  return i == local.accepted_lookup.end()
             ? std::optional<ProteinSchemaAliasBinding>{}
             : local.accepted[i->second].binding;
}
std::optional<ProteinSchemaAliasBinding>
ProteinSchemaAliasIndex::find_before(Id source, std::string_view name) const {
  const auto &local = *impl->local;
  if (auto binding = find(name); binding &&
                                 binding->document == local.document.get() &&
                                 binding->source < source)
    return binding;
  const auto &events = local.registration_parents;
  const auto end = std::upper_bound(
      events.begin(), events.end(), source,
      [](Id id, const auto &event) { return id < event.first; });
  if (end == events.begin() || !std::prev(end)->second)
    return {};
  const auto &visible = std::prev(end)->second->visible;
  const auto i = visible.find(name);
  return i == visible.end() ? std::optional<ProteinSchemaAliasBinding>{}
                            : i->second;
}
ProteinSchemaAliasIndex ProteinSchemaAliasIndex::with_parent_aliases(
    const ProteinSchemaAliasIndex &parent,
    ProteinSchemaAliasOptions options) const {
  return ProteinSchemaAliasIndex(
      std::make_shared<Impl>(impl->local, parent.impl, options));
}
std::vector<ProteinSchemaNamedAlias>
ProteinSchemaAliasIndex::enumerate(uint32_t limit) const {
  size_t count = 0;
  for (auto p = impl.get(); p; p = p->parent.get()) {
    detail::require(p->local->accepted.size() <= limit - count,
                    "Protein Schema enumerated alias limit");
    count += p->local->accepted.size();
  }
  std::vector<ProteinSchemaNamedAlias> result;
  result.reserve(count);
  for (auto p = impl.get(); p; p = p->parent.get())
    for (const auto &[name, binding] : p->local->accepted)
      result.push_back({name, binding});
  return result;
}
ProteinSchemaAliasIndex ProteinSchemaAliasIndex::copy_aliases_from(
    const ProteinSchemaAliasIndex &source,
    ProteinSchemaAliasOptions options) const {
  const auto inputs = source.enumerate(options.max_enumerated_aliases);
  if (inputs.empty())
    return *this;
  detail::require(impl->local->entries.size() <= options.max_aliases &&
                      inputs.size() <=
                          options.max_aliases - impl->local->entries.size(),
                  "Protein Schema local alias limit");
  auto local = std::make_shared<Impl::Local>(*impl->local);
  local->registration_depth =
      std::max(local->registration_depth, source.impl->depth);
  local->imported_sources.push_back(source.impl);
  // Runtime rejections can point into a parent bound after XML input. Retain
  // that snapshot as well, so a later parent replacement cannot dangle them.
  if (impl->parent) {
    local->registration_depth =
        std::max(local->registration_depth, impl->parent->depth);
    local->imported_sources.push_back(impl->parent);
  }
  auto result = std::make_shared<Impl>(local, impl->parent, options);
  std::map<std::string_view, ProteinSchemaAliasBinding, std::less<>> targets;
  for (const auto &[name, binding] : local->accepted)
    targets.emplace(binding.target, binding);
  for (const auto &[name, binding] : inputs) {
    ProteinSchemaAliasEntry entry;
    entry.document = binding.document;
    entry.source = binding.source;
    entry.name = name;
    entry.target = binding.target;
    if (name.empty())
      entry.status = ProteinSchemaAliasStatus::empty_name;
    else if (binding.target.empty())
      entry.status = ProteinSchemaAliasStatus::empty_target;
    else if (auto i = result->visible.find(name); i != result->visible.end()) {
      entry.status = ProteinSchemaAliasStatus::duplicate_name;
      entry.conflict = i->second;
    } else if (auto i = targets.find(binding.target); i != targets.end()) {
      entry.status = ProteinSchemaAliasStatus::duplicate_local_target;
      entry.conflict = i->second;
    } else {
      detail::require(result->visible.size() < options.max_visible_aliases,
                      "Protein Schema visible alias limit");
      entry.status = ProteinSchemaAliasStatus::accepted;
      result->visible.emplace(name, binding);
      targets.emplace(binding.target, binding);
      local->accepted_lookup.emplace(name, local->accepted.size());
      local->accepted.push_back({name, binding});
    }
    local->entries.push_back(entry);
  }
  return ProteinSchemaAliasIndex(std::move(result));
}

namespace {
using DefinitionStatus = ProteinSchemaDefinitionStatus;
using DeclarationStatus = ProteinSchemaDeclarationStatus;
using DeclarationGate = ProteinSchemaDeclarationGate;
using PropertyContainer = ProteinSchemaPropertyContainer;

PropertyContainer schema_container(uint32_t flags) {
  return flags & 2   ? PropertyContainer::readonly_data
         : flags & 8 ? PropertyContainer::iterator_data
                     : PropertyContainer::ordinary;
}
std::string_view schema_effective_id(const ProteinSchemaEntryFields &fields) {
  if (fields.kind == "type")
    return "assettype";
  if (fields.kind == "localtype" || fields.kind == "localname")
    return fields.kind;
  return fields.id.value_or(std::string_view{});
}
void schema_ignored_values(const ProteinSchemaEntryFields &fields,
                           ProteinSchemaValueOptions options) {
  ValueBudget budget{options.max_values};
  auto count = [&](std::string_view input) {
    uint64_t size = 0;
    tokens<false>(input, budget, [&](std::string) { ++size; });
    return size;
  };
  if (fields.kind == "ui" || fields.kind == "implementation")
    budget.use(1);
  else if (fields.value)
    count(*fields.value);
  const bool color = fields.kind == "Color";
  if (!color && fields.kind != "Vector3")
    return;
  auto components = fields.rgba;
  for (size_t c = 0; c < 3; ++c)
    if (fields.xyz[c])
      components[c] = fields.xyz[c];
  std::array<uint64_t, 4> sizes{};
  uint64_t length = color ? 0 : 1;
  const size_t channels = color ? 4 : 3;
  for (size_t c = 0; c < channels; ++c) {
    if (components[c]) {
      sizes[c] = count(*components[c]);
      if (!color)
        sizes[c] = std::min<uint64_t>(sizes[c], 1);
    }
    length = std::max(length, sizes[c]);
  }
  for (size_t c = 0; c < channels; ++c)
    budget.use(length - sizes[c]);
}
// Numeric projection uncertainty can concern a later array value while factory
// existence is known. Only an uncertain first-value/constraint gate stops XML.
bool schema_numeric_gate_known(const ProteinSchemaEntryFields &fields,
                               const ProteinSchemaNumericDefaults &value) {
  if (value.status != ProteinSchemaNumericStatus::unresolved)
    return true;
  if (fields.kind == "Vector3")
    return true;
  if (fields.kind == "Color") {
    if (value.candidates.empty())
      return true;
    return std::all_of(
        value.candidates.begin(), value.candidates.begin() + 4,
        [](const auto &number) { return number.value.has_value(); });
  }
  bool has_range = value.constraint.range.has_value();
  if (value.constraint.status == ProteinSchemaConstraintStatus::unresolved) {
    const auto &raw = *value.constraint.fields;
    if ((raw.minimum && !raw.minimum->value) ||
        (raw.maximum && !raw.maximum->value))
      return false;
    // Increment alone is never part of the first-value contains test.
    has_range = true;
  }
  // No range object means no contains test, even for unresolved numeric text.
  // A present unbounded range still consumes the numeric first value.
  if (!has_range)
    return true;
  if (!value.candidates.empty())
    return value.candidates.front().value.has_value();
  // Omitted val in a multiple declaration has no retained candidates but its
  // first-value gate uses the known default zero.
  return !fields.value;
}
uint64_t schema_cached_values(const ProteinSchemaDefinitionValue &value) {
  return std::visit(
      [](const auto &v) -> uint64_t {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, ProteinSchemaTextDefaults>)
          return v.values.size();
        else if constexpr (std::is_same_v<T, ProteinSchemaNumericDefaults>)
          return v.candidates.size();
        else if constexpr (std::is_same_v<T, ProteinSchemaDefinitionChoice>)
          return v.index.entries().size() + v.defaults.tokens.size() +
                 v.defaults.values.size();
        else
          return 1;
      },
      value);
}
} // namespace

std::optional<ProteinSchemaRuntimeBinding> protein_schema_runtime_binding(
    const ProteinSchemaDefinitionValue &value) noexcept {
  if (value.valueless_by_exception())
    return std::nullopt;
  using Binding = std::optional<ProteinSchemaRuntimeBinding>;
  return std::visit(
      [](const auto &v) -> Binding {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
          return ProteinSchemaRuntimeBinding{"BooleanValue", "bool"};
        } else if constexpr (std::is_same_v<T, ProteinSchemaTextDefaults>) {
          switch (v.value_type) {
          case 2:
            return v.multiple
                       ? ProteinSchemaRuntimeBinding{"StringList",
                                                     "run.string_array"}
                       : ProteinSchemaRuntimeBinding{"StringValue", "string"};
          case 3:
            if (!v.multiple)
              return ProteinSchemaRuntimeBinding{"DateTimeValue", "string"};
            break;
          case 4:
            if (!v.multiple)
              return ProteinSchemaRuntimeBinding{"UriValue", "string"};
            return ProteinSchemaRuntimeBinding{
                v.flags & 0x200 ? "TextureURIValue" : "UriList",
                "run.string_array"};
          case 11:
            if (!v.multiple)
              return ProteinSchemaRuntimeBinding{"UuidValue", "string"};
            break;
          }
        } else if constexpr (std::is_same_v<T, ProteinSchemaNumericDefaults>) {
          if (v.value_type == 6 && v.components == 1)
            return v.multiple
                       ? ProteinSchemaRuntimeBinding{"IntegerList",
                                                     "run.int_array"}
                       : ProteinSchemaRuntimeBinding{"IntegerValue", "int"};
          if (v.value_type == 8 && v.components == 4)
            return v.multiple
                       ? ProteinSchemaRuntimeBinding{"Color4List",
                                                     "run.color4_array"}
                       : ProteinSchemaRuntimeBinding{"Color4Value", "color4"};
          if (v.value_type == 7) {
            if (v.components == 3 && !v.multiple)
              return ProteinSchemaRuntimeBinding{"Vector3Value", "vector3"};
            if (v.components == 1) {
              if (v.multiple)
                return ProteinSchemaRuntimeBinding{"ScalarList",
                                                   "run.double_array"};
              return v.unit.empty()
                         ? ProteinSchemaRuntimeBinding{"ScalarValue", "double"}
                         : ProteinSchemaRuntimeBinding{"GeneralUnitsValue",
                                                       "units_value"};
            }
          }
        } else if constexpr (std::is_same_v<T,
                                            ProteinSchemaConstructedDefaults>) {
          switch (v.kind) {
          case ProteinSchemaConstructedKind::empty_binary:
            return ProteinSchemaRuntimeBinding{"BinaryValue", "byte_buffer"};
          case ProteinSchemaConstructedKind::empty_reference_array:
            return ProteinSchemaRuntimeBinding{"ReferenceValue",
                                               "protein.AssetConnection"};
          case ProteinSchemaConstructedKind::empty_texture_link:
            return ProteinSchemaRuntimeBinding{"TextureValue",
                                               "protein.AssetSymbolLink"};
          default:
            break;
          }
        } else if constexpr (std::is_same_v<T, ProteinSchemaDefinitionChoice>) {
          return v.defaults.multiple
                     ? ProteinSchemaRuntimeBinding{"ChoiceListValue",
                                                   "run.int_array"}
                     : ProteinSchemaRuntimeBinding{"ChoiceValue", "int"};
        }
        return std::nullopt;
      },
      value);
}

namespace {
using Comparison = ProteinSchemaComparisonResult;
using CompareStatus = ProteinSchemaComparisonStatus;
using CompareIssue = ProteinSchemaComparisonIssue;
Comparison compared(bool equal, uint64_t ordinal = 0) {
  return {equal ? CompareStatus::equal : CompareStatus::different,
          CompareIssue::none, ordinal};
}
Comparison uncertain(CompareIssue issue, uint64_t ordinal = 0) {
  return {CompareStatus::unresolved, issue, ordinal};
}
struct SchemaStatePayload {
  enum class Kind {
    invalid,
    integer,
    real,
    color,
    vector,
    units,
    text,
    boolean,
    constructed
  };
  Kind kind = Kind::invalid;
  bool array = false;
  size_t count = 0, stride = 0;
  const ProteinSchemaNumericDefaults *numeric = nullptr;
  const ProteinSchemaTextDefaults *strings = nullptr;
  const ProteinSchemaPropertyState *text_state = nullptr;
  const ProteinSchemaPropertyState *current = nullptr;
  const ProteinSchemaDefinitionChoice *choice = nullptr;
  const bool *boolean = nullptr;
  CompareIssue issue = CompareIssue::none;
  explicit SchemaStatePayload(const ProteinSchemaPropertyState &state) {
    current = &state;
    const auto binding = state.data_binding();
    if (!binding || !state.data()) {
      issue = CompareIssue::unknown_layout;
      return;
    }
    const auto type = binding->data_type;
    if (type == "int" || type == "run.int_array") {
      kind = Kind::integer;
      stride = 4;
    } else if (type == "double" || type == "run.double_array") {
      kind = Kind::real;
      stride = 8;
    } else if (type == "color4" || type == "run.color4_array") {
      kind = Kind::color;
      stride = 32;
    } else if (type == "vector3") {
      kind = Kind::vector;
      stride = 24;
    } else if (type == "units_value") {
      kind = Kind::units;
      stride = 16;
    } else if (type == "string" || type == "run.string_array") {
      kind = Kind::text;
      stride = 16;
    } else if (type == "bool") {
      kind = Kind::boolean;
      stride = 1;
    } else if (std::holds_alternative<ProteinSchemaConstructedDefaults>(
                   *state.data()))
      kind = Kind::constructed;
    else {
      issue = CompareIssue::unknown_layout;
      return;
    }
    array = type == "run.int_array" || type == "run.double_array" ||
            type == "run.color4_array" || type == "run.string_array";
    numeric = std::get_if<ProteinSchemaNumericDefaults>(state.data().get());
    strings = std::get_if<ProteinSchemaTextDefaults>(state.data().get());
    choice = std::get_if<ProteinSchemaDefinitionChoice>(state.data().get());
    boolean = std::get_if<bool>(state.data().get());
    if (numeric) {
      const auto dimensions = kind == Kind::color    ? 4u
                              : kind == Kind::vector ? 3u
                                                     : 1u;
      if (numeric->status == ProteinSchemaNumericStatus::rejected ||
          numeric->status == ProteinSchemaNumericStatus::unsupported)
        issue = CompareIssue::unavailable_projection;
      else if (numeric->components != dimensions ||
               numeric->multiple != array ||
               numeric->candidates.size() % dimensions)
        issue = CompareIssue::inconsistent_shape;
      else
        count = numeric->candidates.size() / dimensions;
    } else if (choice) {
      if (choice->defaults.status != ProteinSchemaChoiceDefaultStatus::ready)
        issue = CompareIssue::unavailable_projection;
      else if (kind != Kind::integer || choice->defaults.multiple != array)
        issue = CompareIssue::inconsistent_shape;
      else
        count = choice->defaults.values.size();
    } else if (strings) {
      if (strings->status != ProteinSchemaTextStatus::ready)
        issue = CompareIssue::unavailable_projection;
      else if (strings->multiple != array ||
               (array && strings->uses_type_default) ||
               (!array &&
                (strings->uses_type_default ? !strings->values.empty()
                                            : strings->values.size() != 1)))
        issue = CompareIssue::inconsistent_shape;
      else
        count = array ? strings->values.size() : 1;
      text_state = &state;
    } else if (boolean || kind == Kind::constructed)
      count = 1;
    else
      issue = CompareIssue::unknown_layout;
    // A full explicit int32 write can establish a scalar value whose old
    // factory conversion was unresolved; original diagnostics remain in data().
    const auto patches = state.pod_patches();
    if (issue == CompareIssue::unavailable_projection && !array &&
        kind == Kind::integer && !patches.empty() &&
        patches.front().byte_offset == 0) {
      count = 1;
      issue = CompareIssue::none;
    }
    if (issue == CompareIssue::none &&
        ((!array && count != 1) ||
         (stride && count > std::numeric_limits<size_t>::max() / stride)))
      issue = CompareIssue::inconsistent_shape;
  }
  std::optional<double> backing_number(size_t component) const {
    if (numeric && component < numeric->candidates.size())
      return numeric->candidates[component].value;
    if (choice && component < choice->defaults.values.size())
      return choice->defaults.values[component];
    return std::nullopt;
  }
  std::optional<double> number(size_t component) const {
    if (!current->pod_patches().empty() &&
        (kind == Kind::integer || kind == Kind::real || kind == Kind::color)) {
      const auto width = kind == Kind::integer ? 4u : 8u;
      if (component > std::numeric_limits<size_t>::max() / width)
        return std::nullopt;
      const auto read = current->pod_bits(component * width, width);
      if (!read.bits)
        return std::nullopt;
      return kind == Kind::integer
                 ? double(std::bit_cast<int32_t>(uint32_t(*read.bits)))
                 : std::bit_cast<double>(*read.bits);
    }
    return backing_number(component);
  }
  std::string_view string(size_t ordinal) const {
    return text_state->text_value(ordinal)->value;
  }
  std::optional<uint64_t> bits(size_t offset, size_t bytes,
                               CompareIssue &failure) const {
    const auto read = current->pod_bits(offset, uint32_t(bytes));
    failure = read.issue;
    return read.bits;
  }
  std::optional<uint64_t> backing_bits(size_t offset, size_t bytes,
                                       CompareIssue &failure) const {
    if (offset > count * stride || bytes > count * stride - offset) {
      failure = CompareIssue::unsafe_raw_access;
      return std::nullopt;
    }
    if (kind != Kind::integer && kind != Kind::real && kind != Kind::color) {
      failure = CompareIssue::raw_layout_required;
      return std::nullopt;
    }
    const size_t word_size = kind == Kind::integer ? 4 : 8;
    uint64_t result = 0;
    for (size_t done = 0; done < bytes;) {
      const auto position = offset + done;
      const auto value = backing_number(position / word_size);
      if (!value) {
        failure = CompareIssue::unknown_number;
        return std::nullopt;
      }
      uint64_t word;
      if (kind == Kind::integer) {
        if (!std::isfinite(*value) ||
            *value < std::numeric_limits<int32_t>::min() ||
            *value > std::numeric_limits<int32_t>::max() ||
            *value != std::trunc(*value)) {
          failure = CompareIssue::inconsistent_shape;
          return std::nullopt;
        }
        word = static_cast<uint32_t>(static_cast<int32_t>(*value));
      } else
        word = std::bit_cast<uint64_t>(*value);
      const auto within = position % word_size;
      const auto take = std::min(bytes - done, word_size - within);
      const auto mask = take == 8 ? std::numeric_limits<uint64_t>::max()
                                  : (uint64_t{1} << (take * 8)) - 1;
      result |= ((word >> (within * 8)) & mask) << (done * 8);
      done += take;
    }
    return result;
  }
};
struct SchemaComparison {
  ProteinSchemaComparisonOptions limits;
  uint64_t values = 0, bytes = 0;
  void value() {
    detail::require(values < limits.max_values,
                    "Protein Schema comparison value limit");
    ++values;
  }
  void text_bytes(size_t count) {
    detail::require(count <= limits.max_text_bytes - bytes,
                    "Protein Schema comparison text limit");
    bytes += count;
  }
  Comparison text(std::string_view a, std::string_view b, uint64_t ordinal) {
    value();
    text_bytes(a.size());
    text_bytes(b.size());
    return compared(a == b, ordinal);
  }
  Comparison choice_table(const ProteinSchemaChoiceIndex &a,
                          const ProteinSchemaChoiceIndex &b) {
    // XML creates a fresh ADSKChoiceList whose header string is empty in both
    // generations. Declaration id/label are not that header. Only accepted
    // entries belong to its vector; unknown entries stop the known prefix.
    const auto left = a.entries(), right = b.entries();
    size_t li = 0, ri = 0;
    auto next = [&](auto rows, size_t &i,
                    bool &unknown) -> const ProteinSchemaChoiceEntry * {
      while (i < rows.size()) {
        value();
        const auto &entry = rows[i++];
        using S = ProteinSchemaChoiceStatus;
        switch (entry.status) {
        case S::accepted:
          return &entry;
        case S::unresolved_attributes:
        case S::unresolved_value:
        case S::unresolved_previous:
          unknown = true;
          return nullptr;
        default:
          break;
        }
      }
      return nullptr;
    };
    for (uint64_t ordinal = 0;; ++ordinal) {
      bool unknown = false;
      const auto *l = next(left, li, unknown);
      if (unknown)
        return uncertain(CompareIssue::incomplete_choice, ordinal);
      if (!l)
        return compared(true);
      const auto *r = next(right, ri, unknown);
      if (unknown)
        return uncertain(CompareIssue::incomplete_choice, ordinal);
      if (!r)
        return uncertain(CompareIssue::short_choice_table, ordinal);
      auto result = text(*l->id, *r->id, ordinal);
      if (result.status != CompareStatus::equal)
        return result;
      value();
      if (*l->value.value != *r->value.value)
        return compared(false, ordinal);
      result = text(*l->label, *r->label, ordinal);
      if (result.status != CompareStatus::equal)
        return result;
    }
  }
  Comparison numeric(const ProteinSchemaNumericDefaults &a,
                     const ProteinSchemaNumericDefaults &b) {
    using S = ProteinSchemaNumericStatus;
    if (a.status == S::rejected || b.status == S::rejected ||
        a.status == S::unsupported || b.status == S::unsupported)
      return uncertain(CompareIssue::unavailable_projection);
    const auto dim = a.components;
    if (!dim || dim != b.components || a.multiple != b.multiple ||
        a.candidates.size() % dim || b.candidates.size() % dim ||
        (!a.multiple &&
         (a.candidates.size() != dim || b.candidates.size() != dim)))
      return uncertain(CompareIssue::inconsistent_shape);
    const auto ac = a.candidates.size() / dim, bc = b.candidates.size() / dim;
    for (size_t i = 0; i < std::min(ac, bc); ++i) {
      for (uint32_t c = 0; c < dim; ++c) {
        value();
        const auto x = a.candidates[i * dim + c].value,
                   y = b.candidates[i * dim + c].value;
        if (!x || !y)
          return uncertain(CompareIssue::unknown_number, i);
        if (a.value_type == 6) {
          auto valid = [](double n) {
            return std::isfinite(n) &&
                   n >= std::numeric_limits<int32_t>::min() &&
                   n <= std::numeric_limits<int32_t>::max() &&
                   n == std::trunc(n);
          };
          if (!valid(*x) || !valid(*y))
            return uncertain(CompareIssue::inconsistent_shape, i);
          if (*x != *y)
            return compared(false, i);
        } else if (a.value_type == 8) {
          const double difference = *x - *y;
          // Both directional color less() searches stop at this same NaN
          // component. Later components of this color are not inspected.
          if (std::isnan(difference))
            break;
          if (difference != 0)
            return compared(false, i);
        } else if (dim == 1 && !a.unit.empty()) {
          const double difference = *x - *y;
          if (difference < 0 || difference > 0)
            return compared(false, i);
          if (!a.unit_code || !b.unit_code)
            return uncertain(CompareIssue::unknown_unit, i);
          if (*a.unit_code != *b.unit_code)
            return compared(false, i);
        } else if (*x < *y || *x > *y) {
          return compared(false, i);
        }
      }
    }
    return compared(ac == bc, std::min(ac, bc));
  }
  Comparison compare_values(const ProteinSchemaDefinitionValue &a,
                            const ProteinSchemaDefinitionValue &b,
                            bool choice_checked = false) {
    const auto at = protein_schema_runtime_binding(a),
               bt = protein_schema_runtime_binding(b);
    if (!at || !bt)
      return uncertain(CompareIssue::unknown_layout);
    if (at->type_name != bt->type_name)
      return compared(false);
    if (a.index() != b.index())
      return uncertain(CompareIssue::inconsistent_shape);
    if (const auto *x = std::get_if<bool>(&a)) {
      value();
      return compared(*x == std::get<bool>(b));
    }
    if (const auto *x = std::get_if<ProteinSchemaNumericDefaults>(&a))
      return numeric(*x, std::get<ProteinSchemaNumericDefaults>(b));
    if (const auto *x = std::get_if<ProteinSchemaTextDefaults>(&a)) {
      const auto &y = std::get<ProteinSchemaTextDefaults>(b);
      if (x->status != ProteinSchemaTextStatus::ready ||
          y.status != ProteinSchemaTextStatus::ready)
        return uncertain(CompareIssue::unavailable_projection);
      auto valid = [](const auto &v) {
        return v.multiple ? !v.uses_type_default
                          : (v.uses_type_default ? v.values.empty()
                                                 : v.values.size() == 1);
      };
      if (!valid(*x) || !valid(y))
        return uncertain(CompareIssue::inconsistent_shape);
      if (x->uses_type_default || y.uses_type_default) {
        // All supported scalar text factories construct their member(2) with
        // the same empty internal-string constructor before optional tokens.
        return text(x->uses_type_default ? std::string_view{} : x->values[0],
                    y.uses_type_default ? std::string_view{} : y.values[0], 0);
      }
      for (size_t i = 0; i < std::min(x->values.size(), y.values.size()); ++i) {
        auto result = text(x->values[i], y.values[i], i);
        if (result.status != CompareStatus::equal)
          return result;
      }
      return compared(x->values.size() == y.values.size(),
                      std::min(x->values.size(), y.values.size()));
    }
    if (std::holds_alternative<ProteinSchemaConstructedDefaults>(a)) {
      value();
      return compared(
          true); // binary empty; Reference/Texture custom class check
    }
    const auto &x = std::get<ProteinSchemaDefinitionChoice>(a),
               &y = std::get<ProteinSchemaDefinitionChoice>(b);
    if (!choice_checked) {
      auto result = choice_table(x.index, y.index);
      if (result.status != CompareStatus::equal)
        return result;
    }
    if (x.defaults.status != ProteinSchemaChoiceDefaultStatus::ready ||
        y.defaults.status != ProteinSchemaChoiceDefaultStatus::ready)
      return uncertain(CompareIssue::unavailable_projection);
    if (!x.defaults.multiple &&
        (x.defaults.values.size() != 1 || y.defaults.values.size() != 1))
      return uncertain(CompareIssue::inconsistent_shape);
    for (size_t i = 0;
         i < std::min(x.defaults.values.size(), y.defaults.values.size());
         ++i) {
      value();
      if (x.defaults.values[i] != y.defaults.values[i])
        return compared(false, i);
    }
    return compared(
        x.defaults.values.size() == y.defaults.values.size(),
        std::min(x.defaults.values.size(), y.defaults.values.size()));
  }
  Comparison state_values(const ProteinSchemaPropertyState &a,
                          const ProteinSchemaPropertyState &b,
                          bool choice_checked = true) {
    const auto at = a.binding(), bt = b.binding();
    if (!at || !bt)
      return uncertain(CompareIssue::unknown_layout);
    if (!a.control_byte() || !b.control_byte())
      return uncertain(CompareIssue::unknown_layout);
    if (a.control_byte() != b.control_byte())
      return compared(false);
    if (at->type_name != bt->type_name)
      return compared(false);
    if (!choice_checked && (at->type_name == "ChoiceValue" ||
                            at->type_name == "ChoiceListValue")) {
      if (!a.choice_table() || !b.choice_table())
        return compared(false);
      const auto result = choice_table(*a.choice_table(), *b.choice_table());
      if (result.status != CompareStatus::equal)
        return result;
    }
    SchemaStatePayload x(a), y(b);
    if (x.issue != CompareIssue::none)
      return uncertain(x.issue);
    if (y.issue != CompareIssue::none)
      return uncertain(y.issue);
    if (x.array != y.array)
      return uncertain(CompareIssue::inconsistent_shape);
    if (x.numeric && y.numeric && a.pod_patches().empty() &&
        b.pod_patches().empty() &&
        a.data_binding()->data_type == b.data_binding()->data_type)
      return numeric(*x.numeric, *y.numeric);
    using K = SchemaStatePayload::Kind;
    for (size_t i = 0; i < std::min(x.count, y.count); ++i) {
      if (x.kind == K::text) {
        if (y.kind != K::text) {
          value();
          return uncertain(CompareIssue::raw_layout_required, i);
        }
        auto result = text(x.string(i), y.string(i), i);
        if (result.status != CompareStatus::equal)
          return result;
      } else if (x.kind == K::boolean || x.kind == K::constructed) {
        value();
        if (x.kind != y.kind)
          return uncertain(CompareIssue::unknown_layout, i);
        if (x.boolean && (!y.boolean || *x.boolean != *y.boolean))
          return compared(false, i);
      } else {
        const auto dimensions = x.kind == K::color    ? 4u
                                : x.kind == K::vector ? 3u
                                                      : 1u;
        for (uint32_t c = 0; c < dimensions; ++c) {
          value();
          const auto left = x.number(i * dimensions + c);
          if (!left)
            return uncertain(CompareIssue::unknown_number, i);
          if (y.kind == K::text)
            return uncertain(CompareIssue::raw_layout_required, i);
          CompareIssue failure = CompareIssue::none;
          const auto raw =
              y.bits(i * y.stride + c * (x.kind == K::integer ? 4 : 8),
                     x.kind == K::integer ? 4 : 8, failure);
          if (!raw)
            return uncertain(failure, i);
          if (x.kind == K::integer) {
            if (!std::isfinite(*left) ||
                *left < std::numeric_limits<int32_t>::min() ||
                *left > std::numeric_limits<int32_t>::max() ||
                *left != std::trunc(*left))
              return uncertain(CompareIssue::inconsistent_shape, i);
            const auto right =
                std::bit_cast<int32_t>(static_cast<uint32_t>(*raw));
            if (*left != right)
              return compared(false, i);
          } else {
            const auto right = std::bit_cast<double>(*raw);
            if (x.kind == K::color) {
              const double difference = *left - right;
              if (std::isnan(difference))
                break;
              if (difference != 0)
                return compared(false, i);
            } else if (*left < right || *left > right)
              return compared(false, i);
          }
        }
      }
    }
    if (!x.array &&
        (x.kind == K::text || x.kind == K::boolean || x.kind == K::constructed))
      return compared(true);
    return compared(x.count == y.count, std::min(x.count, y.count));
  }
  struct Range {
    bool known = true, present = false;
    std::optional<double> low = 0, high = 0, increment = 1;
    std::array<bool, 5> flags{false, true, false, true, false};
  };
  static Range range(const ProteinSchemaResolvedConstraint &v) {
    Range r;
    using S = ProteinSchemaConstraintStatus;
    if (v.status == S::absent || v.status == S::discarded)
      return r;
    r.present = true;
    if (v.status == S::ready && v.range) {
      const auto &f = *v.range;
      r.low = f.minimum.value_or(0);
      r.high = f.maximum.value_or(0);
      r.increment = f.increment;
      r.flags = {f.minimum.has_value(), f.minimum_inclusive,
                 f.maximum.has_value(), f.maximum_inclusive, f.percentage};
    } else if (v.status == S::unresolved && v.fields) {
      const auto &f = *v.fields;
      // Two unknown/partly unknown bounds can still discard the object.
      if (f.minimum && f.maximum && (!f.minimum->value || !f.maximum->value))
        r.known = false;
      r.low = f.minimum ? f.minimum->value : std::optional<double>(0);
      r.high = f.maximum ? f.maximum->value : std::optional<double>(0);
      r.increment = f.increment.value;
      r.flags = {f.minimum.has_value(), f.minimum_inclusive,
                 f.maximum.has_value(), f.maximum_inclusive, f.percentage};
    } else
      r.known = false;
    return r;
  }
  Comparison bounds(const ProteinSchemaResolvedConstraint &a,
                    const ProteinSchemaResolvedConstraint &b) {
    const auto x = range(a), y = range(b);
    if (!x.known || !y.known)
      return uncertain(CompareIssue::unknown_constraint);
    if (!x.present)
      return y.present ? uncertain(CompareIssue::null_left_constraint)
                       : compared(true);
    if (!y.present)
      return compared(false);
    value();
    if (x.flags[0] != y.flags[0])
      return compared(false);
    for (const auto &[l, r] :
         {std::pair{x.low, y.low}, std::pair{x.high, y.high},
          std::pair{x.increment, y.increment}}) {
      value();
      if (!l || !r)
        return uncertain(CompareIssue::unknown_constraint);
      if (!(*l == *r))
        return compared(false); // bounds reject NaN, unlike scalar values
    }
    for (size_t i = 1; i < 5; ++i) {
      value();
      if (x.flags[i] != y.flags[i])
        return compared(false);
    }
    return compared(true);
  }
};
} // namespace

ProteinSchemaComparisonResult
protein_schema_compare_values(const ProteinSchemaDefinitionValue &a,
                              const ProteinSchemaDefinitionValue &b,
                              ProteinSchemaComparisonOptions options) {
  return SchemaComparison{options}.compare_values(a, b);
}
ProteinSchemaComparisonResult
protein_schema_compare_choice_tables(const ProteinSchemaChoiceIndex &a,
                                     const ProteinSchemaChoiceIndex &b,
                                     ProteinSchemaComparisonOptions options) {
  return SchemaComparison{options}.choice_table(a, b);
}
ProteinSchemaComparisonResult
protein_schema_compare_properties(const ProteinSchemaDefinitionProperty *a,
                                  const ProteinSchemaDefinitionProperty *b,
                                  uint32_t skip_type,
                                  ProteinSchemaComparisonOptions options) {
  if (!a || !b || a->value_type != b->value_type)
    return compared(false);
  if (a->value_type == skip_type)
    return compared(true);
  if (!a->value || !b->value)
    return uncertain(CompareIssue::missing_value);
  SchemaComparison compare{options};
  if (a->value_type == 6 || a->value_type == 7) {
    const auto *x = std::get_if<ProteinSchemaNumericDefaults>(a->value.get());
    const auto *y = std::get_if<ProteinSchemaNumericDefaults>(b->value.get());
    if (!x || !y)
      return uncertain(CompareIssue::unknown_layout);
    const auto result = compare.bounds(x->constraint, y->constraint);
    if (result.status != CompareStatus::equal)
      return result;
  } else if (a->value_type == 10) {
    const auto *x = std::get_if<ProteinSchemaDefinitionChoice>(a->value.get());
    const auto *y = std::get_if<ProteinSchemaDefinitionChoice>(b->value.get());
    if (!x || !y)
      return uncertain(CompareIssue::unknown_layout);
    const auto result = compare.choice_table(x->index, y->index);
    if (result.status != CompareStatus::equal)
      return result;
  }
  return compare.compare_values(*a->value, *b->value, a->value_type == 10);
}

struct ProteinSchemaDefinitionIndex::Impl {
  struct Local {
    ProteinSchemaAliasIndex aliases;
    std::vector<ProteinSchemaDefinitionDeclaration> declarations;
    std::string uid;
    Id uid_source = none;
    size_t processed_aliases = 0;
    uint64_t cached_values = 0;
    static ProteinSchemaAliasIndex
    checked_aliases(const ProteinSchemaDocument &doc,
                    const ProteinSchemaAliasIndex *parent,
                    const ProteinSchemaDefinitionOptions &options,
                    std::span<const ProteinSchemaAliasParentChange> changes) {
      detail::require(doc.entries().size() <= options.max_declarations,
                      "Protein Schema definition declaration limit");
      return ProteinSchemaAliasIndex(doc, parent, options.aliases, changes);
    }
    Local(const ProteinSchemaDocument &doc,
          const ProteinSchemaAliasIndex *parent,
          const ProteinSchemaDefinitionOptions &options,
          std::span<const ProteinSchemaAliasParentChange> changes)
        : aliases(checked_aliases(doc, parent, options, changes)) {}
  };
  std::shared_ptr<const Local> local;
  ProteinSchemaAliasIndex visible_aliases;
  std::vector<std::shared_ptr<const Impl>> parents;
  std::vector<ProteinSchemaDefinitionProperty> properties;
  std::array<std::map<std::string_view, Id, std::less<>>, 3> tables;
  std::array<std::vector<Id>, 3> containers;
  DefinitionStatus status = DefinitionStatus::ready;
  Id stopped_before = none;
  uint32_t depth = 1;
  uint64_t extra_cached_values = 0;
  struct GeneratedNames {
    std::string id, label;
  };
  std::vector<std::shared_ptr<const GeneratedNames>> generated_names;
  uint64_t generated_text_bytes = 0;
  Id insert_version(uint32_t, const ProteinSchemaDefinitionOptions &);
  Id insert_reflected(const ProteinSchemaPropertyState &,
                      const ProteinSchemaReflectedCreationOptions &);

  const ProteinSchemaDocument &document() const {
    return local->aliases.document();
  }
  Id direct(std::string_view name) const {
    for (const auto &table : tables)
      if (auto i = table.find(name); i != table.end())
        return i->second;
    return none;
  }
  Id before(Id source, std::string_view name) const {
    if (const auto found = direct(name); found != none)
      return found;
    if (const auto alias = local->aliases.find_before(source, name))
      return direct(alias->target);
    return none;
  }
  Id insert(ProteinSchemaDefinitionProperty property,
            const ProteinSchemaDefinitionOptions &options) {
    detail::require(properties.size() < options.max_property_history,
                    "Protein Schema property history limit");
    property.container = schema_container(property.flags);
    property.active = true;
    auto &table = tables[static_cast<size_t>(property.container)];
    if (auto i = table.find(property.id); i != table.end()) {
      properties[i->second].active = false;
      property.replaced = i->second;
    }
    const Id id = static_cast<Id>(properties.size());
    properties.push_back(std::move(property));
    table.insert_or_assign(properties.back().id, id);
    return id;
  }
  void finish() {
    for (size_t c = 0; c < tables.size(); ++c) {
      containers[c].clear();
      containers[c].reserve(tables[c].size());
      for (const auto &[name, id] : tables[c]) {
        (void)name;
        containers[c].push_back(id);
      }
    }
  }
  Impl(const ProteinSchemaDocument &doc, const ProteinSchemaAliasIndex *parent,
       const ProteinSchemaDefinitionOptions &options,
       std::span<const ProteinSchemaAliasParentChange> changes)
      : local(std::make_shared<Local>(doc, parent, options, changes)),
        visible_aliases(local->aliases) {
    detail::require(options.max_inheritance_depth != 0,
                    "Protein Schema inheritance depth limit");
    // Local is uniquely mutable while constructing this immutable snapshot.
    auto state = std::const_pointer_cast<Local>(local);
    const auto &owned = document();
    state->declarations.reserve(owned.entries().size());
    auto cache = [&](ProteinSchemaDefinitionValue value) {
      const auto count = schema_cached_values(value);
      detail::require(state->cached_values <= options.max_cached_values &&
                          count <=
                              options.max_cached_values - state->cached_values,
                      "Protein Schema cached value limit");
      state->cached_values += count;
      return std::make_shared<const ProteinSchemaDefinitionValue>(
          std::move(value));
    };
    auto ignored = [&](Id source) {
      // Enforce the same per-declaration input limit even when a gate rejects
      // it. Defaults also bounds component normalization for Color/Vector3.
      schema_ignored_values(owned.entry_fields(source), options.values);
    };
    auto create = [&](ProteinSchemaDefinitionDeclaration &row,
                      std::string_view label, uint32_t flags, uint32_t type) {
      ProteinSchemaDefinitionProperty p;
      p.document = p.flags_document = &owned;
      p.source = p.flags_source = row.source;
      p.id = row.effective_id;
      p.label = label;
      p.flags = flags;
      p.value_type = type;
      p.value = row.candidate;
      row.property = insert(std::move(p), options);
      row.status = DeclarationStatus::property;
    };
    for (Id source : owned.entries()) {
      ProteinSchemaDefinitionDeclaration row;
      row.source = source;
      const auto fields = owned.entry_fields(source);
      const auto kind = fields.kind;
      row.effective_id = schema_effective_id(fields);
      if (stopped_before != none) {
        state->declarations.push_back(std::move(row));
        continue;
      }
      // Base's special handler executes before the ordinary declaration gate.
      if (kind == "Base") {
        row.identity = owned.identity_assignment(source, {}, options.values);
        if (row.identity->status == ProteinSchemaIdentityStatus::ready) {
          const auto prior = before(source, "BaseSchema");
          std::string_view current;
          bool safe = true;
          if (prior != none) {
            const auto *text = std::get_if<ProteinSchemaTextDefaults>(
                properties[prior].value.get());
            safe = text && text->value_type == 2 && !text->multiple;
            if (safe && !text->values.empty())
              current = text->values.front();
          }
          if (!safe) {
            row.status = DeclarationStatus::unresolved;
            status = DefinitionStatus::unresolved;
            stopped_before = source;
            state->declarations.push_back(std::move(row));
            continue;
          }
          if (!current.empty()) {
            row.identity->status = ProteinSchemaIdentityStatus::redefined;
            row.identity->value.clear();
            row.identity->flags = 0;
          } else {
            ProteinSchemaTextDefaults text;
            text.status = ProteinSchemaTextStatus::ready;
            text.value_type = 2;
            text.flags = 8;
            text.values.push_back(row.identity->value);
            row.candidate = cache(std::move(text));
            row.effective_id = "BaseSchema";
            create(row, fields.label.value_or(std::string_view{}), 8, 2);
          }
        }
      }
      if (row.effective_id.empty())
        row.gate = DeclarationGate::empty_id;
      else if (const auto found = before(source, row.effective_id);
               found != none) {
        row.gate = DeclarationGate::existing_property;
        row.gate_conflict = found;
      } else {
        // A name not in the exact type map leaves the native initialized type0.
        row.flag_conflicts = protein_schema_flag_conflicts(
            protein_schema_value_type(kind).value_or(0),
            fields.declaration_flags);
        row.gate = row.flag_conflicts ? DeclarationGate::flag_conflicts
                                      : DeclarationGate::accepted;
      }
      if (kind == "UID") {
        row.identity =
            owned.identity_assignment(source, state->uid, options.values);
        if (row.identity->status == ProteinSchemaIdentityStatus::ready) {
          detail::require(state->cached_values < options.max_cached_values,
                          "Protein Schema cached value limit");
          ++state->cached_values;
          state->uid = row.identity->value;
          state->uid_source = source;
          row.status = DeclarationStatus::metadata;
        } else
          row.status = DeclarationStatus::rejected;
      } else if (kind == "PropertyAlias") {
        ignored(source);
        row.alias = static_cast<Id>(state->processed_aliases++);
        row.status = state->aliases.entries()[row.alias].status ==
                             ProteinSchemaAliasStatus::accepted
                         ? DeclarationStatus::alias
                         : DeclarationStatus::rejected;
      } else if (row.property != none) {
        // Successful Base already made a property; its ordinary follow-up
        // observes that property and returns no additional object.
      } else if (row.gate != DeclarationGate::accepted) {
        ignored(source);
        row.status = DeclarationStatus::rejected;
      } else {
        const auto prepared =
            protein_schema_prepare_fields(fields, row.effective_id);
        uint32_t type = 0;
        bool accepted = true, known = true;
        if (kind == "Integer" || kind == "Float" || kind == "Distance" ||
            kind == "Color" || kind == "Vector3") {
          auto value = owned.numeric_defaults(source, options.values);
          accepted = value.status != ProteinSchemaNumericStatus::rejected;
          known = schema_numeric_gate_known(fields, value);
          type = value.value_type;
          row.candidate = cache(std::move(value));
        } else if (kind == "Choice") {
          ProteinSchemaChoiceIndex choices(owned, source, options.values);
          auto value = choices.defaults(options.values);
          known = value.status == ProteinSchemaChoiceDefaultStatus::ready;
          row.candidate = cache(ProteinSchemaDefinitionChoice{
              std::move(choices), std::move(value)});
          type = 10;
        } else if (kind == "Boolean") {
          auto value = owned.defaults(source, options.values);
          row.candidate = cache(*value.boolean_value);
          type = 5;
        } else if (kind == "Binary" || kind == "Reference") {
          row.candidate =
              cache(owned.constructed_defaults(source, options.values));
          type = kind == "Binary" ? 1 : fields.type == "texture" ? 9 : 12;
        } else {
          auto value = owned.text_defaults(source, options.values);
          if (value.status == ProteinSchemaTextStatus::unsupported) {
            ignored(source);
            row.status = DeclarationStatus::unsupported;
            status = DefinitionStatus::unsupported;
            state->declarations.push_back(std::move(row));
            continue;
          }
          accepted = value.status == ProteinSchemaTextStatus::ready;
          type = value.value_type;
          row.candidate = cache(std::move(value));
        }
        if (!known) {
          row.status = DeclarationStatus::unresolved;
          status = DefinitionStatus::unresolved;
          stopped_before = source;
        } else if (accepted)
          create(row, prepared.label, prepared.flags, type);
        else
          row.status = DeclarationStatus::rejected;
      }
      state->declarations.push_back(std::move(row));
    }
    finish();
  }
};

ProteinSchemaDefinitionIndex::ProteinSchemaDefinitionIndex(
    const ProteinSchemaDocument &doc, const ProteinSchemaAliasIndex *parent,
    ProteinSchemaDefinitionOptions options,
    std::span<const ProteinSchemaAliasParentChange> changes)
    : impl(std::make_shared<Impl>(doc, parent, options, changes)) {}
ProteinSchemaDefinitionIndex::ProteinSchemaDefinitionIndex(
    std::shared_ptr<const Impl> p)
    : impl(std::move(p)) {}
const ProteinSchemaDocument &
ProteinSchemaDefinitionIndex::document() const noexcept {
  return impl->document();
}
ProteinSchemaDefinitionStatus
ProteinSchemaDefinitionIndex::status() const noexcept {
  return impl->status;
}
std::span<const ProteinSchemaDefinitionDeclaration>
ProteinSchemaDefinitionIndex::declarations() const noexcept {
  return impl->local->declarations;
}
std::span<const ProteinSchemaAliasEntry>
ProteinSchemaDefinitionIndex::aliases() const noexcept {
  return impl->local->aliases.entries().first(impl->local->processed_aliases);
}
std::optional<ProteinSchemaAliasBinding>
ProteinSchemaDefinitionIndex::find_alias(std::string_view name) const {
  return impl->stopped_before == none
             ? impl->visible_aliases.find(name)
             : impl->local->aliases.find_before(impl->stopped_before, name);
}
std::vector<ProteinSchemaNamedAlias>
ProteinSchemaDefinitionIndex::enumerate_aliases(uint32_t limit) const {
  detail::require(impl->status == ProteinSchemaDefinitionStatus::ready,
                  "Protein Schema alias enumeration unresolved");
  return impl->visible_aliases.enumerate(limit);
}
std::string_view ProteinSchemaDefinitionIndex::uid() const noexcept {
  return impl->local->uid;
}
Id ProteinSchemaDefinitionIndex::uid_source() const noexcept {
  return impl->local->uid_source;
}
std::span<const ProteinSchemaDefinitionProperty>
ProteinSchemaDefinitionIndex::properties() const noexcept {
  return impl->properties;
}
std::span<const Id> ProteinSchemaDefinitionIndex::container(
    ProteinSchemaPropertyContainer group) const {
  const auto i = static_cast<uint32_t>(group);
  detail::require(i < impl->containers.size(),
                  "Protein Schema property container");
  return impl->containers[i];
}
std::vector<Id> ProteinSchemaDefinitionIndex::iteration(
    ProteinSchemaIterationOptions options) const {
  detail::require(
      options.profile == ProteinSchemaIterationProfile::reader_2017 ||
          options.profile == ProteinSchemaIterationProfile::reader_2026,
      "Protein Schema iteration profile");
  const bool old =
      options.profile == ProteinSchemaIterationProfile::reader_2017;
  const auto less = [&](Id a, Id b) {
    const auto left = impl->properties[a].id, right = impl->properties[b].id;
    if (!old)
      return left < right;
    const auto count = std::min(left.size(), right.size());
    for (size_t i = 0; i < count; ++i) {
      const auto x = static_cast<unsigned char>(left[i]);
      const auto y = static_cast<unsigned char>(right[i]);
      if (x != y)
        return (x ^ 0x80u) < (y ^ 0x80u);
    }
    return left.size() < right.size();
  };
  std::array<std::vector<Id>, 3> reordered;
  std::array<std::span<const Id>, 3> groups;
  std::array<size_t, 3> positions{};
  size_t total = 0;
  for (size_t i = 0; i < groups.size(); ++i) {
    groups[i] = impl->containers[i];
    // ASCII tables already have the same order in both profiles. Even mixed
    // tables may already be ordered; allocate only when a reordering is needed.
    if (old && !std::is_sorted(groups[i].begin(), groups[i].end(), less)) {
      reordered[i].assign(groups[i].begin(), groups[i].end());
      std::sort(reordered[i].begin(), reordered[i].end(), less);
      groups[i] = reordered[i];
    }
    positions[i] = options.reverse ? groups[i].size() : 0;
    total += groups[i].size();
  }
  const auto available = [&](size_t i) {
    return options.reverse ? positions[i] != 0
                           : positions[i] < groups[i].size();
  };
  const auto current = [&](size_t i) {
    return groups[i][options.reverse ? positions[i] - 1 : positions[i]];
  };
  std::vector<Id> result;
  result.reserve(total);
  size_t selected = 0;
  for (;;) {
    // Retain the previous group on equal names. Exhausted groups are replaced
    // by the first available group, then all candidates are compared strictly.
    size_t next = available(selected) ? selected : groups.size();
    for (size_t i = 0; i < groups.size(); ++i)
      if (available(i) && (next == groups.size() ||
                           (options.reverse ? less(current(next), current(i))
                                            : less(current(i), current(next)))))
        next = i;
    if (next == groups.size())
      break;
    selected = next;
    const Id id = current(selected);
    if (!options.public_only || (impl->properties[id].flags & 1))
      result.push_back(id);
    if (options.reverse)
      --positions[selected];
    else
      ++positions[selected];
  }
  return result;
}
ProteinSchemaDefinitionLookup
ProteinSchemaDefinitionIndex::find(std::string_view name) const {
  auto found = impl->direct(name);
  if (found == none)
    if (const auto alias = find_alias(name))
      found = impl->direct(alias->target);
  if (found != none)
    return {ProteinSchemaDefinitionLookupStatus::found, found};
  return {impl->status == DefinitionStatus::ready
              ? ProteinSchemaDefinitionLookupStatus::missing
              : ProteinSchemaDefinitionLookupStatus::unresolved,
          none};
}
ProteinSchemaDefinitionLookup
ProteinSchemaDefinitionIndex::find_actual(std::string_view name) const {
  const auto found = impl->direct(name);
  if (found != none)
    return {ProteinSchemaDefinitionLookupStatus::found, found};
  return {impl->status == DefinitionStatus::ready
              ? ProteinSchemaDefinitionLookupStatus::missing
              : ProteinSchemaDefinitionLookupStatus::unresolved,
          none};
}
ProteinSchemaDefinitionIndex
ProteinSchemaDefinitionIndex::with_inherited_parent(
    const ProteinSchemaDefinitionIndex &parent,
    ProteinSchemaDefinitionOptions options) const {
  detail::require(impl->status == DefinitionStatus::ready &&
                      parent.impl->status == DefinitionStatus::ready,
                  "Protein Schema inheritance requires known acceptance");
  detail::require(std::max(impl->depth, parent.impl->depth) <
                      options.max_inheritance_depth,
                  "Protein Schema inheritance depth limit");
  detail::require(
      impl->properties.size() <= options.max_property_history &&
          impl->local->declarations.size() <= options.max_declarations &&
          impl->local->cached_values <= options.max_cached_values &&
          impl->extra_cached_values <=
              options.max_cached_values - impl->local->cached_values,
      "Protein Schema definition snapshot limit");
  auto result = std::make_shared<Impl>(*impl);
  result->depth = std::max(impl->depth, parent.impl->depth) + 1;
  result->parents.push_back(parent.impl);
  result->visible_aliases = impl->visible_aliases.with_parent_aliases(
      parent.impl->visible_aliases, options.aliases);
  for (const auto &group : parent.impl->containers)
    for (Id id : group) {
      const auto &p = parent.impl->properties[id];
      const auto previous = result->direct(p.id);
      if (previous == none) {
        auto copy = p;
        copy.previous = copy.replaced = none;
        result->insert(std::move(copy), options);
      } else {
        auto copy = result->properties[previous];
        const auto old_container = copy.container;
        copy.flags = p.flags | (copy.flags & 0x180);
        copy.flags_document = p.flags_document;
        copy.flags_source = p.flags_source;
        copy.previous = previous;
        copy.replaced = none;
        result->properties[previous].active = false;
        const auto new_id = result->insert(std::move(copy), options);
        if (result->properties[new_id].container != old_container)
          result->tables[static_cast<size_t>(old_container)].erase(p.id);
      }
    }
  result->finish();
  return ProteinSchemaDefinitionIndex(std::move(result));
}
struct ProteinSchemaDefinitionLoadIndex::Impl {
  using Index = ProteinSchemaDefinitionIndex;
  using RecordStatus = ProteinSchemaDefinitionLoadRecordStatus;
  using Issue = ProteinSchemaDefinitionLoadIssue;
  using EventKind = ProteinSchemaDefinitionLoadEventKind;
  std::vector<Index> inputs;
  std::vector<std::optional<Index>> definitions;
  std::vector<ProteinSchemaDefinitionLoadRecord> records;
  std::vector<ProteinSchemaDefinitionLoadEvent> events;
  std::map<std::string_view, Id, std::less<>> ready, waiting;
  std::vector<Id> pending;
  std::vector<ProteinSchemaDefinitionLoadCycle> cycles;
  std::vector<ProteinSchemaCommonRequirement> requirements;
  ProteinSchemaDefinitionLoadStatus status =
      ProteinSchemaDefinitionLoadStatus::dependencies_resolved;
  ProteinSchemaDefinitionLoadOptions options;
  uint64_t history = 0, cached = 0;

  static void charge(uint64_t &used, uint64_t count, uint64_t limit,
                     const char *message) {
    detail::require(used <= limit && count <= limit - used, message);
    used += count;
  }
  void event(EventKind kind, Id record, std::string_view name = {},
             Id property = none, Id previous = none, Id related = none) {
    detail::require(events.size() < options.max_events,
                    "Protein Schema load event limit");
    events.push_back({kind, record, property, previous, related, name});
  }
  void halt(Id id, Issue issue, Id property = none,
            std::string_view name = {}) {
    auto &r = records[id];
    r.status = RecordStatus::unresolved;
    r.issue = issue;
    r.issue_property = property;
    status = ProteinSchemaDefinitionLoadStatus::unresolved;
    event(EventKind::unresolved, id, name, property);
  }
  void replace(Id id, Index next) {
    // Inputs and current outputs are both retained. A shared input/output Impl
    // is charged once; a distinct output history is charged in full.
    const auto &old = definitions[id];
    if (old && old->impl != inputs[id].impl)
      history -= old->properties().size();
    if (next.impl != inputs[id].impl)
      charge(history, next.properties().size(),
             options.max_total_property_history,
             "Protein Schema total property history limit");
    definitions[id] = std::move(next);
  }
  bool prepare(Id id) {
    auto &r = records[id];
    definitions[id] = inputs[id];
    if (inputs[id].status() != DefinitionStatus::ready) {
      halt(id, Issue::uncertain_local_definition);
      return false;
    }
    std::shared_ptr<Index::Impl> changed;
    auto mutable_state = [&]() -> Index::Impl & {
      if (!changed) {
        changed = std::make_shared<Index::Impl>(*inputs[id].impl);
        replace(id, Index(changed));
      }
      return *changed;
    };
    auto finish = [&] {
      if (changed)
        changed->finish();
    };
    auto cache = [&](ProteinSchemaDefinitionValue value) {
      auto &state = mutable_state();
      const auto count = schema_cached_values(value);
      detail::require(state.local->cached_values <=
                              options.definition.max_cached_values &&
                          state.extra_cached_values <=
                              options.definition.max_cached_values -
                                  state.local->cached_values,
                      "Protein Schema cached value limit");
      charge(state.extra_cached_values, count,
             options.definition.max_cached_values - state.local->cached_values,
             "Protein Schema cached value limit");
      charge(cached, count, options.max_total_cached_values,
             "Protein Schema total cached value limit");
      return std::make_shared<const ProteinSchemaDefinitionValue>(
          std::move(value));
    };
    auto insert = [&](ProteinSchemaDefinitionProperty property) {
      auto &state = mutable_state();
      charge(history, 1, options.max_total_property_history,
             "Protein Schema total property history limit");
      return state.insert(std::move(property), options.definition);
    };
    auto default_property = [&](std::string_view name, uint32_t type,
                                uint32_t flags) {
      const auto found = definitions[id]->find(name);
      if (found.status == ProteinSchemaDefinitionLookupStatus::found)
        return found.property;
      ProteinSchemaDefinitionProperty property;
      property.document = property.flags_document = &inputs[id].document();
      property.origin = ProteinSchemaPropertyOrigin::load_default;
      property.id = name;
      property.value_type = type;
      property.flags = flags;
      property.label = flags & 1 ? name : std::string_view{};
      if (type == 6) {
        ProteinSchemaNumericDefaults value;
        value.status = ProteinSchemaNumericStatus::ready;
        value.value_type = 6;
        value.components = 1;
        value.candidates.push_back(default_number(1));
        property.value = cache(std::move(value));
      } else {
        ProteinSchemaTextDefaults value;
        value.status = ProteinSchemaTextStatus::ready;
        value.value_type = type;
        value.flags = flags;
        value.values.emplace_back();
        property.value = cache(std::move(value));
      }
      const auto result = insert(std::move(property));
      event(EventKind::default_property, id, name, result);
      return result;
    };
    const auto version = default_property("SchemaVersion", 6, 2);
    if (r.uid != "CommonSchema") {
      finish();
      return true;
    }
    default_property("AssetLibID", 2, 1);
    default_property("VersionGUID", 11, 8);
    default_property("localtype", 2, 0x12);
    default_property("localname", 2, 0x12);
    const auto *number = std::get_if<ProteinSchemaNumericDefaults>(
        definitions[id]->properties()[version].value.get());
    if (!number || number->value_type != 6 || number->components != 1) {
      finish();
      halt(id, Issue::incompatible_version, version, "SchemaVersion");
      return false;
    }
    if (number->multiple && number->candidates.empty()) {
      r.common_version = 0;
      r.version_dimension_invalid = true;
      event(EventKind::version_invalid_dimension, id, "SchemaVersion", version);
    } else if (number->candidates.empty() ||
               !number->candidates.front().value) {
      finish();
      halt(id, Issue::uncertain_version, version, "SchemaVersion");
      return false;
    } else {
      r.common_version =
          static_cast<int32_t>(*number->candidates.front().value);
    }
    if (*r.common_version < 3) {
      for (auto name :
           {std::string_view("category"), std::string_view("keyword")}) {
        const auto found = definitions[id]->find(name);
        if (found.status != ProteinSchemaDefinitionLookupStatus::found) {
          finish();
          halt(id, Issue::missing_common_text, none, name);
          return false;
        }
        auto property = definitions[id]->properties()[found.property];
        const auto *text =
            std::get_if<ProteinSchemaTextDefaults>(property.value.get());
        if (!text || text->value_type != 2) {
          finish();
          halt(id, Issue::incompatible_common_text, found.property, name);
          return false;
        }
        ProteinSchemaTextDefaults value;
        value.status = text->status;
        value.value_type = text->value_type;
        value.flags = text->flags;
        value.multiple = text->multiple;
        value.values.emplace_back();
        property.value = cache(std::move(value));
        property.origin = ProteinSchemaPropertyOrigin::load_rewrite;
        property.previous = found.property;
        property.replaced = none;
        const auto updated = insert(std::move(property));
        event(EventKind::rewrite_property, id, name, updated, found.property);
      }
    }
    finish();
    return true;
  }
  void diagnose_pending() {
    for (const auto &[name, id] : waiting) {
      (void)name;
      pending.push_back(id);
      if (auto i = waiting.find(records[id].base); i != waiting.end())
        records[id].waiting_on = i->second;
    }
    std::vector<uint8_t> color(records.size());
    std::vector<Id> path;
    for (Id start : pending) {
      if (color[start])
        continue;
      path.clear();
      Id current = start;
      while (current != none && !color[current]) {
        color[current] = 1;
        path.push_back(current);
        current = records[current].waiting_on;
      }
      if (current != none && color[current] == 1) {
        const auto first = std::find(path.begin(), path.end(), current);
        detail::require(first != path.end(),
                        "Protein Schema pending cycle state");
        ProteinSchemaDefinitionLoadCycle cycle;
        cycle.records.assign(first, path.end());
        const auto ordinal = static_cast<Id>(cycles.size());
        for (Id member : cycle.records)
          records[member].cycle = ordinal;
        cycles.push_back(std::move(cycle));
      }
      for (Id member : path)
        color[member] = 2;
    }
  }
  void check_common() {
    const auto found = ready.find("CommonSchema");
    if (found == ready.end())
      return;
    const Id id = found->second;
    const auto &state = *definitions[id]->impl;
    static constexpr std::pair<std::string_view, uint32_t> expected[]{
        {"BaseSchema", 8},
        {"SchemaVersion", 2},
        {"AssetLibID", 0},
        {"assettype", 2},
        {"localtype", 2},
        {"localname", 2},
        {"UIName", 8},
        {"keyword", 12},
        {"category", 12},
        {"description", 8},
        {"thumbnail", 12},
        {"Hidden", 8},
        {"VersionGUID", 8},
        {"version", 0},
        {"revision", 0},
        {"UIDefinition", 2},
        {"AdvancedUIDefinition", 2}};
    for (const auto &[name, flags] : expected) {
      ProteinSchemaCommonRequirement requirement;
      requirement.record = id;
      requirement.name = name;
      requirement.required_flags = flags;
      requirement.property = state.direct(name);
      if (requirement.property != none)
        requirement.status =
            (state.properties[requirement.property].flags & 14) == flags
                ? ProteinSchemaCommonRequirementStatus::matched
                : ProteinSchemaCommonRequirementStatus::flags_mismatch;
      requirements.push_back(requirement);
    }
  }
  Impl(std::span<const Index> sources,
       ProteinSchemaDefinitionLoadOptions limits)
      : options(limits) {
    detail::require(sources.size() <= options.max_sources,
                    "Protein Schema load source limit");
    uint64_t declarations = 0;
    for (const auto &source : sources) {
      detail::require(source.impl->parents.empty() && source.impl->depth == 1 &&
                          source.impl->extra_cached_values == 0,
                      "Protein Schema load requires local definition inputs");
      charge(declarations, source.declarations().size(),
             options.max_total_declarations,
             "Protein Schema total declaration limit");
      charge(history, source.properties().size(),
             options.max_total_property_history,
             "Protein Schema total property history limit");
      charge(cached, source.impl->local->cached_values,
             options.max_total_cached_values,
             "Protein Schema total cached value limit");
      detail::require(source.properties().size() <=
                              options.definition.max_property_history &&
                          source.declarations().size() <=
                              options.definition.max_declarations &&
                          source.impl->local->cached_values <=
                              options.definition.max_cached_values &&
                          source.impl->depth <=
                              options.definition.max_inheritance_depth,
                      "Protein Schema input definition limit");
    }
    inputs.assign(sources.begin(), sources.end());
    records.resize(inputs.size());
    definitions.resize(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i)
      records[i].uid = inputs[i].uid();
    std::map<std::string_view, std::vector<Id>, std::less<>> by_base;
    std::map<std::string_view, Id, std::less<>> available;
    auto promote = [&](Id id, Id parent) {
      auto &r = records[id];
      if (parent != none) {
        replace(id, definitions[id]->with_inherited_parent(*definitions[parent],
                                                           options.definition));
        r.parent = parent;
      }
      r.status = RecordStatus::resolved;
      ready.emplace(r.uid, id);
      event(EventKind::resolved, id, r.uid, none, none, parent);
      if (auto children = by_base.find(r.uid); children != by_base.end()) {
        for (Id child : children->second)
          available.emplace(records[child].uid, child);
        by_base.erase(children);
      }
    };
    auto drain = [&] {
      while (!available.empty()) {
        const auto id = available.begin()->second;
        available.erase(available.begin());
        const auto parent = ready.find(records[id].base);
        detail::require(parent != ready.end(),
                        "Protein Schema ready dependency state");
        promote(id, parent->second);
        waiting.erase(records[id].uid);
      }
    };
    for (Id id = 0; id < inputs.size(); ++id) {
      if (status == ProteinSchemaDefinitionLoadStatus::unresolved)
        break;
      auto &r = records[id];
      // Even duplicate XML must have a known completed local acceptance path.
      // Unknown native behavior is not silently bypassed by a matching UID.
      if (inputs[id].status() != DefinitionStatus::ready) {
        definitions[id] = inputs[id];
        halt(id, Issue::uncertain_local_definition);
        break;
      }
      const auto registered = ready.find(r.uid), queued = waiting.find(r.uid);
      if (registered != ready.end() || queued != waiting.end()) {
        r.status = RecordStatus::duplicate;
        r.duplicate_was_pending = registered == ready.end();
        r.canonical =
            r.duplicate_was_pending ? queued->second : registered->second;
        event(EventKind::duplicate, id, r.uid, none, none, r.canonical);
        continue;
      }
      if (!prepare(id))
        break;
      if (r.uid == "CommonSchema") {
        r.canonical = id;
        promote(id, none);
        drain();
        continue;
      }
      const auto found = definitions[id]->find("BaseSchema");
      if (found.status == ProteinSchemaDefinitionLookupStatus::found) {
        const auto *text = std::get_if<ProteinSchemaTextDefaults>(
            definitions[id]->properties()[found.property].value.get());
        if (!text || text->value_type != 2 || text->multiple) {
          halt(id, Issue::incompatible_base, found.property, "BaseSchema");
          break;
        }
        if (!text->values.empty())
          r.base = text->values.front();
      }
      if (r.base.empty()) {
        r.status = RecordStatus::rejected;
        r.issue = Issue::empty_base;
        event(EventKind::rejected, id, "BaseSchema");
        continue;
      }
      r.canonical = id;
      if (const auto parent = ready.find(r.base); parent != ready.end()) {
        promote(id, parent->second);
        drain();
      } else {
        r.status = RecordStatus::pending;
        waiting.emplace(r.uid, id);
        by_base[r.base].push_back(id);
        event(EventKind::queued, id, r.base);
      }
    }
    diagnose_pending();
    check_common();
    if (status != ProteinSchemaDefinitionLoadStatus::unresolved &&
        !pending.empty())
      status = ProteinSchemaDefinitionLoadStatus::pending_dependencies;
  }
};
ProteinSchemaDefinitionLoadIndex::ProteinSchemaDefinitionLoadIndex(
    std::span<const ProteinSchemaDefinitionIndex> inputs,
    ProteinSchemaDefinitionLoadOptions options)
    : impl(std::make_shared<Impl>(inputs, options)) {}
ProteinSchemaDefinitionLoadStatus
ProteinSchemaDefinitionLoadIndex::status() const noexcept {
  return impl->status;
}
std::span<const ProteinSchemaDefinitionLoadRecord>
ProteinSchemaDefinitionLoadIndex::records() const noexcept {
  return impl->records;
}
const ProteinSchemaDefinitionIndex &
ProteinSchemaDefinitionLoadIndex::input(Id record) const {
  detail::require(record < impl->inputs.size(),
                  "Protein Schema load input record");
  return impl->inputs[record];
}
const ProteinSchemaDefinitionIndex *
ProteinSchemaDefinitionLoadIndex::definition(Id record) const {
  detail::require(record < impl->records.size(),
                  "Protein Schema load definition record");
  if (impl->records[record].status ==
      ProteinSchemaDefinitionLoadRecordStatus::duplicate)
    record = impl->records[record].canonical;
  const auto &result = impl->definitions[record];
  return result ? &*result : nullptr;
}
ProteinSchemaDefinitionLoadLookup
ProteinSchemaDefinitionLoadIndex::find_uid(std::string_view name) const {
  if (auto i = impl->ready.find(name); i != impl->ready.end())
    return {ProteinSchemaDefinitionLoadLookupStatus::found, i->second};
  if (auto i = impl->waiting.find(name); i != impl->waiting.end())
    return {ProteinSchemaDefinitionLoadLookupStatus::pending, i->second};
  return {impl->status == ProteinSchemaDefinitionLoadStatus::unresolved
              ? ProteinSchemaDefinitionLoadLookupStatus::unresolved
              : ProteinSchemaDefinitionLoadLookupStatus::missing,
          none};
}
std::span<const ProteinSchemaDefinitionLoadEvent>
ProteinSchemaDefinitionLoadIndex::events() const noexcept {
  return impl->events;
}
std::span<const Id> ProteinSchemaDefinitionLoadIndex::pending() const noexcept {
  return impl->pending;
}
std::span<const ProteinSchemaDefinitionLoadCycle>
ProteinSchemaDefinitionLoadIndex::cycles() const noexcept {
  return impl->cycles;
}
std::span<const ProteinSchemaCommonRequirement>
ProteinSchemaDefinitionLoadIndex::common_requirements() const noexcept {
  return impl->requirements;
}
struct ProteinSchemaPropertyState::Impl {
  struct Seed {
    ProteinSchemaDefinitionIndex definition;
    Id property;
    Seed(const ProteinSchemaDefinitionIndex &index, Id row)
        : definition(index), property(row) {}
    const ProteinSchemaDefinitionProperty &get() const noexcept {
      return definition.properties()[property];
    }
  };
  using Owner = std::shared_ptr<const Seed>;
  struct TextOverride {
    size_t ordinal;
    Owner owner;
    size_t source_ordinal, prefix_length;
  };
  using TextOverrides = std::vector<TextOverride>;
  std::shared_ptr<const TextOverrides> text_overrides;
  std::shared_ptr<const std::vector<ProteinSchemaPodPatch>> pod_patches;
  std::optional<uint32_t> flags_override;
  Owner identity, data, flags, label, constraint, choice, control;
  std::optional<uint8_t> control_byte;
  ProteinSchemaReferencePresence constraint_presence =
      ProteinSchemaReferencePresence::absent;
  Impl(const ProteinSchemaDefinitionIndex &index, Id row) {
    detail::require(row < index.properties().size(),
                    "Protein Schema property state row out of range");
    identity = data = flags = label = std::make_shared<const Seed>(index, row);
    if (identity->get().constructed_binding ||
        (identity->get().value &&
         protein_schema_runtime_binding(*identity->get().value))) {
      control = identity;
      control_byte = 1;
    }
    if (const auto &value = data->get().value) {
      if (const auto *numeric =
              std::get_if<ProteinSchemaNumericDefaults>(value.get())) {
        const auto status = numeric->constraint.status;
        if (status == ProteinSchemaConstraintStatus::ready ||
            status == ProteinSchemaConstraintStatus::unresolved) {
          constraint = data;
          constraint_presence =
              status == ProteinSchemaConstraintStatus::ready
                  ? ProteinSchemaReferencePresence::present
                  : ProteinSchemaReferencePresence::unresolved;
        }
      }
      if (std::holds_alternative<ProteinSchemaDefinitionChoice>(*value))
        choice = data;
    }
  }
};

ProteinSchemaPropertyState::ProteinSchemaPropertyState(
    std::shared_ptr<const Impl> state)
    : impl(std::move(state)) {}
ProteinSchemaPropertyState::ProteinSchemaPropertyState(
    const ProteinSchemaDefinitionIndex &index, Id property)
    : impl(std::make_shared<const Impl>(index, property)) {}
const ProteinSchemaDefinitionProperty &
ProteinSchemaPropertyState::target() const noexcept {
  return impl->identity->get();
}
std::optional<ProteinSchemaRuntimeBinding>
ProteinSchemaPropertyState::binding() const noexcept {
  if (target().constructed_binding)
    return target().constructed_binding;
  const auto &value = target().value;
  return value ? protein_schema_runtime_binding(*value) : std::nullopt;
}
const std::shared_ptr<const ProteinSchemaDefinitionValue> &
ProteinSchemaPropertyState::data() const noexcept {
  return impl->data->get().value;
}
std::optional<ProteinSchemaRuntimeBinding>
ProteinSchemaPropertyState::data_binding() const noexcept {
  return data() ? protein_schema_runtime_binding(*data()) : std::nullopt;
}
std::optional<size_t>
ProteinSchemaPropertyState::text_value_count() const noexcept {
  const auto binding = data_binding();
  const auto *text =
      data() ? std::get_if<ProteinSchemaTextDefaults>(data().get()) : nullptr;
  if (!binding || !text || text->status != ProteinSchemaTextStatus::ready)
    return std::nullopt;
  const bool array = binding->data_type == "run.string_array";
  if ((!array && binding->data_type != "string") || text->multiple != array ||
      (array && text->uses_type_default) ||
      (!array && (text->uses_type_default ? !text->values.empty()
                                          : text->values.size() != 1)))
    return std::nullopt;
  return array ? text->values.size() : 1;
}
std::optional<ProteinSchemaPropertyTextValue>
ProteinSchemaPropertyState::text_value(size_t ordinal) const noexcept {
  const auto count = text_value_count();
  if (!count || ordinal >= *count)
    return std::nullopt;
  auto owner = impl->data.get();
  auto source_ordinal = ordinal;
  std::optional<size_t> prefix;
  if (impl->text_overrides) {
    const auto &rows = *impl->text_overrides;
    const auto i = std::lower_bound(
        rows.begin(), rows.end(), ordinal,
        [](const auto &row, size_t value) { return row.ordinal < value; });
    if (i != rows.end() && i->ordinal == ordinal) {
      owner = i->owner.get();
      source_ordinal = i->source_ordinal;
      prefix = i->prefix_length;
    }
  }
  const auto &text = std::get<ProteinSchemaTextDefaults>(*owner->get().value);
  const auto value = text.uses_type_default
                         ? std::string_view{}
                         : std::string_view(text.values[source_ordinal]);
  return ProteinSchemaPropertyTextValue{prefix ? value.substr(0, *prefix)
                                               : value,
                                        {&owner->definition, owner->property},
                                        source_ordinal,
                                        prefix};
}
bool ProteinSchemaPropertyState::has_text_overrides() const noexcept {
  return bool(impl->text_overrides);
}
std::span<const ProteinSchemaPodPatch>
ProteinSchemaPropertyState::pod_patches() const noexcept {
  return impl->pod_patches ? std::span(*impl->pod_patches)
                           : std::span<const ProteinSchemaPodPatch>{};
}
ProteinSchemaPropertyFieldSource
ProteinSchemaPropertyState::backing_supplier() const noexcept {
  return {&impl->data->definition, impl->data->property};
}
ProteinSchemaPodRead
ProteinSchemaPropertyState::pod_bits(size_t offset, uint32_t bytes) const {
  if (!bytes || bytes > 8)
    return {std::nullopt, CompareIssue::unsafe_raw_access};
  const SchemaStatePayload payload(*this);
  if (payload.issue != CompareIssue::none)
    return {std::nullopt, payload.issue};
  if (offset > payload.count * payload.stride ||
      bytes > payload.count * payload.stride - offset)
    return {std::nullopt, CompareIssue::unsafe_raw_access};
  using K = SchemaStatePayload::Kind;
  if (payload.kind != K::integer && payload.kind != K::real &&
      payload.kind != K::color)
    return {std::nullopt, CompareIssue::raw_layout_required};
  const auto patches = pod_patches();
  if (patches.empty()) {
    CompareIssue failure = CompareIssue::none;
    const auto value = payload.backing_bits(offset, bytes, failure);
    return {value, failure};
  }
  uint64_t result = 0;
  auto patch = std::upper_bound(
      patches.begin(), patches.end(), offset,
      [](size_t at, const auto &p) { return at < p.byte_offset; });
  if (patch != patches.begin() && offset - (patch - 1)->byte_offset < 4)
    --patch;
  for (size_t done = 0; done < bytes;) {
    const auto at = offset + done;
    while (patch != patches.end() && at >= patch->byte_offset &&
           at - patch->byte_offset >= 4)
      ++patch;
    uint64_t word;
    size_t take;
    if (patch != patches.end() && patch->byte_offset <= at) {
      const auto within = at - patch->byte_offset;
      take = std::min<size_t>(bytes - done, 4 - within);
      word = uint64_t(patch->bits) >> (within * 8);
    } else {
      take = bytes - done;
      if (patch != patches.end())
        take = std::min(take, patch->byte_offset - at);
      CompareIssue failure = CompareIssue::none;
      const auto original = payload.backing_bits(at, take, failure);
      if (!original)
        return {std::nullopt, failure};
      word = *original;
    }
    const auto mask = take == 8 ? std::numeric_limits<uint64_t>::max()
                                : (uint64_t{1} << (take * 8)) - 1;
    result |= (word & mask) << (done * 8);
    done += take;
  }
  return {result, CompareIssue::none};
}
std::optional<uint8_t>
ProteinSchemaPropertyState::control_byte() const noexcept {
  return impl->control_byte;
}
ProteinSchemaPropertyState
ProteinSchemaPropertyState::with_control_byte(uint8_t value) const {
  detail::require(impl->control_byte.has_value(),
                  "Unknown Protein Schema control byte layout");
  if (impl->control_byte == value && !impl->control)
    return *this;
  auto result = std::make_shared<Impl>(*impl);
  result->control_byte = value;
  result->control.reset();
  return ProteinSchemaPropertyState(std::move(result));
}
uint32_t ProteinSchemaPropertyState::flags() const noexcept {
  return impl->flags_override ? *impl->flags_override
                              : impl->flags->get().flags;
}
ProteinSchemaPropertyState
ProteinSchemaPropertyState::with_flags(uint32_t value) const {
  detail::require(impl->control_byte.has_value(),
                  "Unknown Protein Schema flags layout");
  if (impl->flags_override == value)
    return *this;
  auto result = std::make_shared<Impl>(*impl);
  result->flags_override = value;
  return ProteinSchemaPropertyState(std::move(result));
}
std::string_view ProteinSchemaPropertyState::label() const noexcept {
  return impl->label->get().label;
}
ProteinSchemaReferencePresence
ProteinSchemaPropertyState::constraint_presence() const noexcept {
  return impl->constraint_presence;
}
const ProteinSchemaResolvedConstraint *
ProteinSchemaPropertyState::constraint() const noexcept {
  return impl->constraint ? &std::get<ProteinSchemaNumericDefaults>(
                                 *impl->constraint->get().value)
                                 .constraint
                          : nullptr;
}
const ProteinSchemaChoiceIndex *
ProteinSchemaPropertyState::choice_table() const noexcept {
  return impl->choice ? &std::get<ProteinSchemaDefinitionChoice>(
                             *impl->choice->get().value)
                             .index
                      : nullptr;
}
ProteinSchemaPropertyFieldSource
ProteinSchemaPropertyState::supplier(ProteinSchemaPropertyField field) const {
  const Impl::Owner *owner = nullptr;
  switch (field) {
  case ProteinSchemaPropertyField::identity:
    owner = &impl->identity;
    break;
  case ProteinSchemaPropertyField::data:
    if (impl->text_overrides || impl->pod_patches)
      return {};
    owner = &impl->data;
    break;
  case ProteinSchemaPropertyField::flags:
    if (impl->flags_override)
      return {};
    owner = &impl->flags;
    break;
  case ProteinSchemaPropertyField::label:
    owner = &impl->label;
    break;
  case ProteinSchemaPropertyField::constraint:
    owner = &impl->constraint;
    break;
  case ProteinSchemaPropertyField::choice:
    owner = &impl->choice;
    break;
  case ProteinSchemaPropertyField::control:
    owner = &impl->control;
    break;
  default:
    detail::require(false, "Invalid Protein Schema property state field");
  }
  return *owner ? ProteinSchemaPropertyFieldSource{&(*owner)->definition,
                                                   (*owner)->property}
                : ProteinSchemaPropertyFieldSource{};
}
namespace {
ProteinSchemaPropertyCopyIssue
schema_copy_layout_issue(std::optional<ProteinSchemaRuntimeBinding> left,
                         std::optional<ProteinSchemaRuntimeBinding> right) {
  using Issue = ProteinSchemaPropertyCopyIssue;
  if (!left || !right)
    return Issue::unknown_layout;
  // All registered run.*_array projections share the native Array assignment
  // callback. Its source element Type travels with the copied storage.
  const auto array = [](std::string_view type) {
    return type == "run.string_array" || type == "run.int_array" ||
           type == "run.double_array" || type == "run.color4_array";
  };
  if (left->data_type != right->data_type &&
      !(array(left->data_type) && array(right->data_type)))
    return Issue::raw_layout_required;
  return Issue::none;
}
ProteinSchemaPropertyCopyIssue
schema_reflected_copy_issue(const ProteinSchemaPropertyState &source) {
  using I = ProteinSchemaPropertyCopyIssue;
  if (!source.data())
    return I::missing_value;
  const auto issue =
      schema_copy_layout_issue(source.binding(), source.data_binding());
  if (issue != I::none)
    return issue;
  return source.constraint_presence() ==
                 ProteinSchemaReferencePresence::unresolved
             ? I::unknown_constraint
             : I::none;
}
uint64_t schema_reflected_value_cost(const ProteinSchemaPropertyState &source) {
  auto result = schema_cached_values(*source.data());
  const auto *choice =
      std::get_if<ProteinSchemaDefinitionChoice>(source.data().get());
  if (source.choice_table() &&
      (!choice || &choice->index != source.choice_table())) {
    const auto extra = source.choice_table()->entries().size();
    detail::require(extra <= std::numeric_limits<uint64_t>::max() - result,
                    "Protein Schema reflected value cost overflow");
    result += extra;
  }
  return result;
}
} // namespace
ProteinSchemaPropertyCopyResult ProteinSchemaPropertyState::copied_from(
    const ProteinSchemaPropertyState &source) const {
  using Issue = ProteinSchemaPropertyCopyIssue;
  if ((!target().value && !target().constructed_binding) || !source.data())
    return {Issue::missing_value, std::nullopt};
  const auto issue = schema_copy_layout_issue(binding(), source.data_binding());
  if (issue != Issue::none)
    return {issue, std::nullopt};
  if (impl == source.impl)
    return {Issue::none, *this};
  if (source.constraint_presence() ==
      ProteinSchemaReferencePresence::unresolved)
    return {Issue::unknown_constraint, std::nullopt};
  if (impl->data == source.impl->data &&
      impl->text_overrides == source.impl->text_overrides &&
      impl->pod_patches == source.impl->pod_patches &&
      impl->flags_override == source.impl->flags_override &&
      impl->flags == source.impl->flags && impl->label == source.impl->label &&
      (!source.impl->constraint ||
       impl->constraint == source.impl->constraint) &&
      (!source.impl->choice || impl->choice == source.impl->choice))
    return {Issue::none, *this};
  auto result = std::make_shared<Impl>(*impl);
  result->data = source.impl->data;
  result->text_overrides = source.impl->text_overrides;
  result->pod_patches = source.impl->pod_patches;
  result->flags_override = source.impl->flags_override;
  result->flags = source.impl->flags;
  result->label = source.impl->label;
  if (source.impl->constraint) {
    result->constraint = source.impl->constraint;
    result->constraint_presence = source.impl->constraint_presence;
  }
  if (source.impl->choice)
    result->choice = source.impl->choice;
  return {Issue::none, ProteinSchemaPropertyState(std::move(result))};
}
// Internal batch writer. Sparse entries hold indexed-row owners, never states.
struct ProteinSchemaStateAccess {
  static ProteinSchemaPropertyState
  pod_written(const ProteinSchemaPropertyState &target, size_t offset,
              uint32_t bits) {
    using Impl = ProteinSchemaPropertyState::Impl;
    const auto old = target.pod_patches();
    const auto at = std::lower_bound(
        old.begin(), old.end(), offset,
        [](const auto &patch, size_t key) { return patch.byte_offset < key; });
    if (at != old.end() && at->byte_offset == offset && at->bits == bits)
      return target;
    auto rows = std::make_shared<std::vector<ProteinSchemaPodPatch>>();
    rows->reserve(old.size() + (at == old.end() || at->byte_offset != offset));
    rows->insert(rows->end(), old.begin(), at);
    rows->push_back({offset, bits});
    rows->insert(rows->end(),
                 at != old.end() && at->byte_offset == offset ? at + 1 : at,
                 old.end());
    auto result = std::make_shared<Impl>(*target.impl);
    result->pod_patches = std::move(rows);
    return ProteinSchemaPropertyState(std::move(result));
  }
  struct Write {
    size_t ordinal, prefix_length;
  };
  static ProteinSchemaPropertyState
  text_written(const ProteinSchemaPropertyState &target,
               const ProteinSchemaPropertyState &source,
               std::span<const Write> writes) {
    if (writes.empty())
      return target;
    using Impl = ProteinSchemaPropertyState::Impl;
    auto rows = std::make_shared<Impl::TextOverrides>();
    const Impl::TextOverrides empty;
    const auto &previous =
        target.impl->text_overrides ? *target.impl->text_overrides : empty;
    rows->reserve(previous.size() + writes.size());
    auto old = previous.begin();
    const auto *source_rows = source.impl->text_overrides.get();
    auto si = size_t{0};
    for (const auto &write : writes) {
      while (old != previous.end() && old->ordinal < write.ordinal)
        rows->push_back(*old++);
      if (old != previous.end() && old->ordinal == write.ordinal)
        ++old;
      auto owner = source.impl->data;
      auto original = write.ordinal;
      if (source_rows) {
        while (si < source_rows->size() &&
               (*source_rows)[si].ordinal < write.ordinal)
          ++si;
        if (si < source_rows->size() &&
            (*source_rows)[si].ordinal == write.ordinal) {
          owner = (*source_rows)[si].owner;
          original = (*source_rows)[si].source_ordinal;
        }
      }
      rows->push_back(
          {write.ordinal, std::move(owner), original, write.prefix_length});
    }
    rows->insert(rows->end(), old, previous.end());
    auto result = std::make_shared<Impl>(*target.impl);
    result->text_overrides = std::move(rows);
    return ProteinSchemaPropertyState(std::move(result));
  }
};
namespace {
Comparison schema_compare_property_states(const ProteinSchemaPropertyState *a,
                                          const ProteinSchemaPropertyState *b,
                                          uint32_t skip_type,
                                          SchemaComparison &compare) {
  if (!a || !b || a->target().value_type != b->target().value_type)
    return compared(false);
  if (a->target().value_type == skip_type)
    return compared(true);
  if (!a->data() || !b->data())
    return uncertain(CompareIssue::missing_value);
  if (a->target().value_type == 6 || a->target().value_type == 7) {
    if (a->constraint_presence() ==
            ProteinSchemaReferencePresence::unresolved ||
        b->constraint_presence() == ProteinSchemaReferencePresence::unresolved)
      return uncertain(CompareIssue::unknown_constraint);
    const ProteinSchemaResolvedConstraint absent;
    const auto result =
        compare.bounds(a->constraint() ? *a->constraint() : absent,
                       b->constraint() ? *b->constraint() : absent);
    if (result.status != CompareStatus::equal)
      return result;
  } else if (a->target().value_type == 10) {
    if (!a->choice_table() || !b->choice_table())
      return uncertain(CompareIssue::incomplete_choice);
    const auto result =
        compare.choice_table(*a->choice_table(), *b->choice_table());
    if (result.status != CompareStatus::equal)
      return result;
  }
  return compare.state_values(*a, *b);
}
struct SchemaIntegerLayout {
  size_t count = 0, stride = 0;
  bool choice = false, pod = false;
  ProteinSchemaIntegerIssue issue = ProteinSchemaIntegerIssue::none;
  explicit SchemaIntegerLayout(const ProteinSchemaPropertyState &state) {
    using I = ProteinSchemaIntegerIssue;
    const auto binding = state.binding();
    if (!binding) {
      issue = I::unknown_layout;
      return;
    }
    const auto name = binding->type_name;
    choice = name == "ChoiceValue" || name == "ChoiceListValue";
    const bool scalar = name == "IntegerValue" || name == "ChoiceValue";
    if (!scalar && name != "IntegerList" && name != "ChoiceListValue") {
      issue = I::wrong_interface;
      return;
    }
    const auto data = state.data_binding();
    if (!data) {
      issue = I::unknown_layout;
      return;
    }
    if (scalar) {
      count = 1;
      stride = 4;
      pod = data->data_type == "int";
    } else {
      const SchemaStatePayload payload(state);
      if (payload.issue != CompareIssue::none || !payload.array) {
        issue = I::unknown_layout;
        return;
      }
      count = payload.count;
      stride = payload.stride;
      pod = payload.kind == SchemaStatePayload::Kind::integer ||
            payload.kind == SchemaStatePayload::Kind::real ||
            payload.kind == SchemaStatePayload::Kind::color;
    }
  }
};
ProteinSchemaIntegerRead
schema_get_integer(const ProteinSchemaPropertyState &state, uint64_t ordinal,
                   SchemaComparison &compare) {
  using S = ProteinSchemaIntegerStatus;
  using I = ProteinSchemaIntegerIssue;
  const SchemaIntegerLayout layout(state);
  if (layout.issue != I::none)
    return {S::unresolved, layout.issue, std::nullopt};
  if (ordinal >= layout.count)
    return {S::rejected, I::ordinal_out_of_range, 0};
  if (!layout.pod)
    return {S::unresolved, I::raw_layout_required, std::nullopt};
  compare.value();
  const auto read = state.pod_bits(size_t(ordinal) * layout.stride, 4);
  if (!read.bits)
    return {S::unresolved, I::unknown_value, std::nullopt};
  return {S::ready, I::none, uint32_t(*read.bits)};
}
ProteinSchemaIntegerWrite
schema_set_integer(const ProteinSchemaPropertyState &state, uint32_t bits,
                   uint64_t ordinal, SchemaComparison &compare) {
  using S = ProteinSchemaIntegerStatus;
  using I = ProteinSchemaIntegerIssue;
  ProteinSchemaIntegerWrite result{state};
  const SchemaIntegerLayout layout(state);
  if (layout.issue != I::none) {
    result.issue = layout.issue;
    return result;
  }
  if (ordinal >= layout.count) {
    result.status = S::rejected;
    result.issue = I::ordinal_out_of_range;
    return result;
  }
  // Choice membership precedes the actual element data access.
  compare.value();
  if (layout.choice) {
    const auto *table = state.choice_table();
    if (!table) {
      result.issue = I::unknown_choice;
      return result;
    }
    bool found = false;
    for (const auto &entry : table->entries()) {
      compare.value();
      using C = ProteinSchemaChoiceStatus;
      if (entry.status == C::unresolved_attributes ||
          entry.status == C::unresolved_value ||
          entry.status == C::unresolved_previous) {
        result.issue = I::unknown_choice;
        return result;
      }
      if (entry.status == C::accepted && uint32_t(*entry.value.value) == bits) {
        found = true;
        break;
      }
    }
    if (!found) {
      result.status = S::rejected;
      result.issue = I::choice_value_missing;
      return result;
    }
  }
  if (!layout.pod) {
    result.issue = I::raw_layout_required;
    return result;
  }
  result.target = ProteinSchemaStateAccess::pod_written(
      state, size_t(ordinal) * layout.stride, bits);
  result.status = S::ready;
  result.written = true;
  return result;
}
ProteinSchemaPropertyVersionResult
schema_apply_ordinary_version(const ProteinSchemaPropertyState &source,
                              const ProteinSchemaPropertyState &target,
                              uint32_t source_version, uint32_t target_version,
                              SchemaComparison &compare) {
  using Status = ProteinSchemaPropertyVersionStatus;
  using Issue = ProteinSchemaPropertyVersionIssue;
  ProteinSchemaPropertyVersionResult result{target};
  if (source.target().id == "SchemaVersion" ||
      source.target().id == "BaseSchema" || target.target().value_type == 4) {
    result.issue = Issue::special_branch_required;
    return result;
  }
  if (source_version != target_version) {
    if (source.target().value_type != target.target().value_type) {
      result.status = Status::incompatible;
      result.issue = Issue::type_mismatch;
      return result;
    }
    if (source.flags() != target.flags()) {
      result.status = Status::incompatible;
      result.issue = Issue::flags_mismatch;
      return result;
    }
    if (target.target().value_type == 7) {
      const auto unit = [](const ProteinSchemaPropertyState &state)
          -> std::optional<uint32_t> {
        const auto binding = state.binding();
        if (!binding)
          return std::nullopt;
        if (binding->type_name == "ScalarValue" ||
            binding->type_name == "ScalarList" ||
            binding->type_name == "Vector3Value")
          return 0;
        if (binding->type_name == "GeneralUnitsValue" && state.data())
          if (const auto *value =
                  std::get_if<ProteinSchemaNumericDefaults>(state.data().get()))
            return value->unit_code;
        return std::nullopt;
      };
      const auto a = unit(source), b = unit(target);
      if (!a || !b) {
        result.issue = source.binding() && target.binding()
                           ? Issue::unknown_unit
                           : Issue::unknown_layout;
        return result;
      }
      if (a != b) {
        result.status = Status::incompatible;
        result.issue = Issue::unit_mismatch;
        return result;
      }
    }
    if (source_version < target_version) {
      result.status = Status::compatible;
      return result;
    }
    if (!source.control_byte() || !target.control_byte()) {
      result.issue = Issue::unknown_layout;
      return result;
    }
    if (source.control_byte() != target.control_byte()) {
      result.target = target.with_control_byte(*source.control_byte());
      result.control_written = true;
    }
  }
  result.comparison =
      schema_compare_property_states(&source, &result.target, 4, compare);
  if (result.comparison->status == ProteinSchemaComparisonStatus::unresolved) {
    result.issue = Issue::comparison_unresolved;
    return result;
  }
  if (result.comparison->status == ProteinSchemaComparisonStatus::equal) {
    result.status = Status::compatible;
    return result;
  }
  if (source_version == target_version) {
    result.status = Status::incompatible;
    result.issue = Issue::comparison_different;
    return result;
  }
  result.copy_attempted = true;
  auto copied = result.target.copied_from(source);
  result.copy_issue = copied.issue;
  if (!copied.state) {
    result.issue = Issue::copy_unresolved;
    return result;
  }
  result.target = std::move(*copied.state);
  result.copied = true;
  result.status = Status::compatible;
  return result;
}

Comparison
schema_compare_base_properties(const ProteinSchemaPropertyState *target,
                               const ProteinSchemaPropertyState *source,
                               SchemaComparison &compare) {
  if (!target || !source ||
      target->target().value_type != source->target().value_type)
    return compared(false);
  if (!target->control_byte() || !source->control_byte())
    return uncertain(CompareIssue::unknown_layout);
  if (target->control_byte() != source->control_byte() ||
      ((target->flags() ^ source->flags()) & 0xfffffff7u))
    return compared(false);
  if (!target->data() || !source->data())
    return uncertain(CompareIssue::missing_value);
  return compare.state_values(*target, *source, false);
}
ProteinSchemaUriResult
schema_apply_uri(const ProteinSchemaPropertyState &source,
                 const ProteinSchemaPropertyState &target,
                 ProteinSchemaUriContext context, SchemaComparison &compare) {
  using Status = ProteinSchemaUriStatus;
  using Issue = ProteinSchemaUriIssue;
  ProteinSchemaUriResult result{target};
  const auto incompatible = [&](Issue issue) {
    result.status = Status::incompatible;
    result.issue = issue;
    return result;
  };
  if (target.target().value_type != 4 || source.target().value_type != 4)
    return incompatible(Issue::type_mismatch);
  if ((source.flags() | target.flags()) & 0x200u) {
    if (!context.source_context_complete) {
      result.issue = Issue::unknown_context;
      return result;
    }
    if (context.source_context_word == 3 &&
        ((source.flags() ^ target.flags()) & 0xfffffdfbu))
      return incompatible(Issue::flags_mismatch);
  }
  if (!source.control_byte() || !target.control_byte()) {
    result.issue = Issue::unknown_layout;
    return result;
  }
  if (source.control_byte() != target.control_byte())
    return incompatible(Issue::control_mismatch);
  const auto count =
      [](const ProteinSchemaPropertyState &state) -> std::optional<size_t> {
    const auto binding = state.binding();
    if (!binding)
      return std::nullopt;
    if (binding->type_name == "UriValue")
      return 1;
    if (binding->type_name != "UriList" &&
        binding->type_name != "TextureURIValue")
      return std::nullopt;
    const SchemaStatePayload payload(state);
    if (payload.issue != CompareIssue::none || !payload.array)
      return std::nullopt;
    return payload.count;
  };
  const auto sc = count(source), tc = count(target);
  if (!sc || !tc) {
    result.issue = Issue::unknown_layout;
    return result;
  }
  if (*sc != *tc)
    return incompatible(Issue::count_mismatch);
  if (*sc == 0) {
    result.status = Status::compatible;
    return result;
  }
  if (!source.text_value_count() || !target.text_value_count()) {
    result.issue = Issue::raw_layout_required;
    return result;
  }
  // Scan at most one byte past the remaining logical text budget. Embedded NUL
  // ends the visible value; bytes after it are retained but not inspected here.
  const auto visible = [&](std::string_view value) {
    const auto remaining = compare.limits.max_text_bytes - compare.bytes;
    const auto bound =
        remaining < value.size() ? size_t(remaining) + 1 : value.size();
    const auto nul = value.substr(0, bound).find('\0');
    const auto size = nul == std::string_view::npos ? value.size() : nul;
    compare.text_bytes(size);
    return value.substr(0, size);
  };
  const auto relative = [](std::string_view text) {
    return !text.empty() && text.front() != '/' && text.front() != '\\' &&
           text.find(':') == std::string_view::npos;
  };
  std::vector<ProteinSchemaStateAccess::Write> writes;
  for (size_t i = 0; i < *sc; ++i) {
    compare.value();
    const auto a = visible(target.text_value(i)->value);
    const auto b = visible(source.text_value(i)->value);
    if (a != b && !relative(a) && relative(b)) {
      writes.push_back({i, b.size()});
      result.replaced_ordinals.push_back(i);
    }
  }
  result.target =
      ProteinSchemaStateAccess::text_written(target, source, writes);
  result.status = Status::compatible;
  result.ordinal = *sc;
  return result;
}
} // namespace
ProteinSchemaFlagsRepairResult
protein_schema_repair_source_flags(const ProteinSchemaPropertyState &source,
                                   ProteinSchemaSourceContext context) {
  using I = ProteinSchemaFlagsRepairIssue;
  ProteinSchemaFlagsRepairResult result{source};
  if (source.target().id == "SchemaVersion" || !(source.flags() & 0x800u) ||
      context.presence == ProteinSchemaReferencePresence::absent)
    return result;
  if (context.presence != ProteinSchemaReferencePresence::present ||
      !context.word_d0) {
    result.issue = I::unknown_context;
    return result;
  }
  if (*context.word_d0 != 0x100000003ull)
    return result;
  if (!context.word_d8) {
    result.issue = I::unknown_context;
    return result;
  }
  if (*context.word_d8 != 3)
    return result;
  if (!source.control_byte()) {
    result.issue = I::unknown_layout;
    return result;
  }
  result.source = source.with_flags(source.flags() & ~0x800u);
  result.written = true;
  return result;
}
ProteinSchemaIntegerRead
protein_schema_get_integer(const ProteinSchemaPropertyState &state,
                           uint64_t ordinal,
                           ProteinSchemaComparisonOptions options) {
  SchemaComparison compare{options};
  return schema_get_integer(state, ordinal, compare);
}
ProteinSchemaIntegerWrite
protein_schema_set_integer(const ProteinSchemaPropertyState &state,
                           uint32_t bits, uint64_t ordinal,
                           ProteinSchemaComparisonOptions options) {
  SchemaComparison compare{options};
  return schema_set_integer(state, bits, ordinal, compare);
}
ProteinSchemaVersionRead
schema_read_version(const ProteinSchemaPropertyState *property, bool complete,
                    SchemaComparison &compare) {
  using I = ProteinSchemaVersionAccessIssue;
  ProteinSchemaVersionRead result;
  if (!complete) {
    result.issue = I::lookup_unresolved;
    return result;
  }
  if (!property) {
    result.value = 1;
    result.defaulted = true;
    return result;
  }
  result.integer = schema_get_integer(*property, 0, compare);
  result.value = result.integer->bits;
  if (!result.value)
    result.issue = I::integer_unresolved;
  return result;
}
ProteinSchemaVersionFinalResult
schema_finalize_version(const ProteinSchemaPropertyState *property, uint32_t sv,
                        uint32_t tv, bool compatible, bool complete,
                        SchemaComparison &compare) {
  using S = ProteinSchemaVersionFinalStatus;
  using I = ProteinSchemaVersionAccessIssue;
  ProteinSchemaVersionFinalResult result;
  if (property)
    result.target = *property;
  if (!compatible || sv <= tv) {
    result.status = S::unchanged;
    return result;
  }
  if (!complete) {
    result.issue = I::lookup_unresolved;
    return result;
  }
  if (!property) {
    result.creation = ProteinSchemaVersionCreation{"SchemaVersion", "", 2, sv};
    result.status = S::creation_required;
    return result;
  }
  result.write_requested = true;
  result.integer = schema_set_integer(*property, sv, 0, compare);
  result.target = result.integer->target;
  result.written = result.integer->written;
  if (result.integer->status == ProteinSchemaIntegerStatus::unresolved)
    result.issue = I::integer_unresolved;
  else
    result.status = S::finished;
  return result;
}
ProteinSchemaVersionRead
protein_schema_read_version(const ProteinSchemaPropertyState *property,
                            bool complete,
                            ProteinSchemaComparisonOptions options) {
  SchemaComparison compare{options};
  return schema_read_version(property, complete, compare);
}
ProteinSchemaVersionFinalResult protein_schema_finalize_version(
    const ProteinSchemaPropertyState *property, uint32_t sv, uint32_t tv,
    bool compatible, bool complete, ProteinSchemaComparisonOptions options) {
  SchemaComparison compare{options};
  return schema_finalize_version(property, sv, tv, compatible, complete,
                                 compare);
}
Id ProteinSchemaDefinitionIndex::Impl::insert_version(
    uint32_t sv, const ProteinSchemaDefinitionOptions &limits) {
  detail::require(local->cached_values < limits.max_cached_values &&
                      extra_cached_values <
                          limits.max_cached_values - local->cached_values,
                  "Protein Schema cached value limit");
  ProteinSchemaNumericDefaults number;
  number.status = ProteinSchemaNumericStatus::ready;
  number.value_type = 6;
  number.components = 1;
  const auto signed_value =
      sv <= 0x7fffffff ? int64_t(sv) : int64_t(sv) - (int64_t{1} << 32);
  number.candidates.push_back(default_number(double(signed_value)));
  ProteinSchemaDefinitionProperty generated;
  generated.document = generated.flags_document = &document();
  generated.origin = ProteinSchemaPropertyOrigin::registration_create;
  generated.id = "SchemaVersion";
  generated.value_type = 6;
  generated.flags = 2;
  generated.value =
      std::make_shared<const ProteinSchemaDefinitionValue>(std::move(number));
  const auto row = insert(std::move(generated), limits);
  ++extra_cached_values;
  return row;
}
Id ProteinSchemaDefinitionIndex::Impl::insert_reflected(
    const ProteinSchemaPropertyState &source,
    const ProteinSchemaReflectedCreationOptions &options) {
  const auto &limits = options.definition;
  auto charge = [](uint64_t &used, uint64_t count, uint64_t limit,
                   const char *message) {
    detail::require(used <= limit && count <= limit - used, message);
    used += count;
  };
  detail::require(local->cached_values <= limits.max_cached_values,
                  "Protein Schema reflected value limit");
  charge(extra_cached_values, schema_reflected_value_cost(source),
         limits.max_cached_values - local->cached_values,
         "Protein Schema reflected value limit");
  charge(generated_text_bytes, source.target().id.size(),
         options.max_generated_text_bytes,
         "Protein Schema reflected name limit");
  charge(generated_text_bytes, source.label().size(),
         options.max_generated_text_bytes,
         "Protein Schema reflected name limit");
  auto names = std::make_shared<const GeneratedNames>(GeneratedNames{
      std::string(source.target().id), std::string(source.label())});
  ProteinSchemaDefinitionProperty generated;
  generated.document = generated.flags_document = &document();
  generated.origin = ProteinSchemaPropertyOrigin::registration_create;
  generated.id = names->id;
  generated.label = names->label;
  generated.value_type = source.target().value_type;
  generated.flags = source.flags();
  generated.constructed_binding = source.binding();
  const auto row = insert(std::move(generated), limits);
  generated_names.push_back(std::move(names));
  return row;
}
struct ProteinSchemaDefinitionState::Impl {
  ProteinSchemaDefinitionIndex index, original;
  std::shared_ptr<const uint8_t> identity = std::make_shared<const uint8_t>(0);
  std::map<Id, ProteinSchemaPropertyState> updates;
  std::optional<uint8_t> merge_marker = std::nullopt;
  explicit Impl(const ProteinSchemaDefinitionIndex &value)
      : index(value), original(value) {}
};
ProteinSchemaDefinitionState::ProteinSchemaDefinitionState(
    std::shared_ptr<const Impl> value)
    : impl(std::move(value)) {}
ProteinSchemaDefinitionState::ProteinSchemaDefinitionState(
    const ProteinSchemaDefinitionIndex &index)
    : impl(std::make_shared<const Impl>(index)) {}
bool ProteinSchemaDefinitionState::same_identity(
    const ProteinSchemaDefinitionState &other) const noexcept {
  return impl && other.impl && impl->identity == other.impl->identity;
}
std::optional<uint8_t>
ProteinSchemaDefinitionState::merge_marker_byte() const noexcept {
  return impl->merge_marker;
}
ProteinSchemaDefinitionState
ProteinSchemaDefinitionState::with_merge_marker_byte(uint8_t value) const {
  auto result = std::make_shared<Impl>(*impl);
  result->merge_marker = value;
  return ProteinSchemaDefinitionState(std::move(result));
}
std::span<const ProteinSchemaAliasEntry>
ProteinSchemaDefinitionState::alias_attempts() const noexcept {
  return impl->index.impl->status == ProteinSchemaDefinitionStatus::ready
             ? impl->index.impl->visible_aliases.entries()
             : impl->index.aliases();
}
ProteinSchemaDefinitionState ProteinSchemaDefinitionState::copy_aliases_from(
    const ProteinSchemaDefinitionState &source,
    ProteinSchemaAliasOptions options) const {
  detail::require(
      impl->index.status() == ProteinSchemaDefinitionStatus::ready &&
          source.impl->index.status() == ProteinSchemaDefinitionStatus::ready,
      "Protein Schema alias transfer unresolved");
  auto aliases = impl->index.impl->visible_aliases.copy_aliases_from(
      source.impl->index.impl->visible_aliases, options);
  if (aliases.impl == impl->index.impl->visible_aliases.impl)
    return *this;
  auto metadata =
      std::make_shared<ProteinSchemaDefinitionIndex::Impl>(*impl->index.impl);
  metadata->visible_aliases = std::move(aliases);
  auto state = std::make_shared<Impl>(*impl);
  state->index = ProteinSchemaDefinitionIndex(std::move(metadata));
  return ProteinSchemaDefinitionState(std::move(state));
}
const ProteinSchemaDefinitionIndex &
ProteinSchemaDefinitionState::definition() const noexcept {
  return impl->index;
}
ProteinSchemaDefinitionLookup
ProteinSchemaDefinitionState::find_actual(std::string_view name) const {
  return impl->index.find_actual(name);
}
ProteinSchemaDefinitionLookup
ProteinSchemaDefinitionState::find(std::string_view name) const {
  return impl->index.find(name);
}
ProteinSchemaPropertyState
ProteinSchemaDefinitionState::property(Id row) const {
  detail::require(row < impl->index.properties().size(),
                  "Protein Schema definition state row out of range");
  if (auto i = impl->updates.find(row); i != impl->updates.end())
    return i->second;
  return ProteinSchemaPropertyState(impl->original, row);
}
ProteinSchemaDefinitionState ProteinSchemaDefinitionState::with_property(
    Id row, const ProteinSchemaPropertyState &value) const {
  detail::require(&property(row).target() == &value.target(),
                  "Protein Schema definition state target identity mismatch");
  auto result = std::make_shared<Impl>(*impl);
  result->updates.insert_or_assign(row, value);
  return ProteinSchemaDefinitionState(std::move(result));
}
ProteinSchemaVersionRead ProteinSchemaDefinitionState::read_version(
    ProteinSchemaComparisonOptions options) const {
  const auto found = find("SchemaVersion");
  const auto value = found.status == ProteinSchemaDefinitionLookupStatus::found
                         ? std::optional(property(found.property))
                         : std::nullopt;
  return protein_schema_read_version(
      value ? &*value : nullptr,
      found.status != ProteinSchemaDefinitionLookupStatus::unresolved, options);
}
ProteinSchemaScopedVersionResult ProteinSchemaDefinitionState::finalize_version(
    uint32_t sv, uint32_t tv, bool compatible,
    ProteinSchemaComparisonOptions options,
    ProteinSchemaDefinitionOptions limits) const {
  ProteinSchemaScopedVersionResult result{*this, {}};
  if (!compatible || sv <= tv) {
    result.step.status = ProteinSchemaVersionFinalStatus::unchanged;
    return result;
  }
  const auto found = find("SchemaVersion");
  const auto value = found.status == ProteinSchemaDefinitionLookupStatus::found
                         ? std::optional(property(found.property))
                         : std::nullopt;
  result.property = found.property;
  result.step = protein_schema_finalize_version(
      value ? &*value : nullptr, sv, tv, compatible,
      found.status != ProteinSchemaDefinitionLookupStatus::unresolved, options);
  if (value) {
    if (result.step.written)
      result.state = with_property(found.property, *result.step.target);
    return result;
  }
  if (result.step.status != ProteinSchemaVersionFinalStatus::creation_required)
    return result;
  auto metadata =
      std::make_shared<ProteinSchemaDefinitionIndex::Impl>(*impl->index.impl);
  result.property = metadata->insert_version(sv, limits);
  metadata->finish();
  auto state = std::make_shared<Impl>(*impl);
  state->index = ProteinSchemaDefinitionIndex(std::move(metadata));
  ProteinSchemaPropertyState created(state->index, result.property);
  state->updates.emplace(result.property, created);
  result.state = ProteinSchemaDefinitionState(std::move(state));
  result.step.target = created;
  result.step.status = ProteinSchemaVersionFinalStatus::finished;
  result.step.written = true;
  result.created = true;
  return result;
}
ProteinSchemaReflectedCreationResult
ProteinSchemaDefinitionState::create_reflected_properties(
    std::span<const ProteinSchemaPropertyState> sources,
    ProteinSchemaReflectedCreationOptions options) const {
  using Issue = ProteinSchemaPropertyCopyIssue;
  using Metadata = ProteinSchemaDefinitionIndex::Impl;
  ProteinSchemaReflectedCreationResult result{*this, {}};
  const auto &limits = options.definition;
  detail::require(sources.size() <= limits.max_declarations,
                  "Protein Schema reflected source limit");
  std::shared_ptr<Metadata> metadata;
  for (size_t i = 0; i < sources.size(); ++i) {
    const auto &source = sources[i];
    result.issue = schema_reflected_copy_issue(source);
    if (result.issue != Issue::none)
      break;
    if (!metadata)
      metadata = std::make_shared<Metadata>(*impl->index.impl);
    const auto row = metadata->insert_reflected(source, options);
    const auto &property = metadata->properties[row];
    result.properties.push_back(
        {i, row, property.replaced, property.container});
    result.next_source = i + 1;
  }
  if (!metadata)
    return result;
  metadata->finish();
  auto state = std::make_shared<Impl>(*impl);
  state->index = ProteinSchemaDefinitionIndex(std::move(metadata));
  // All created identities retain this one finalized index. No mutable builder
  // escapes, and no source state or earlier output state is retained as a
  // chain.
  for (const auto &entry : result.properties) {
    const ProteinSchemaPropertyState created(state->index, entry.property);
    const auto copy = created.copied_from(sources[entry.source_ordinal]);
    detail::require(copy.state.has_value(),
                    "Protein Schema reflected copy preflight mismatch");
    state->updates.emplace(entry.property, *copy.state);
  }
  result.state = ProteinSchemaDefinitionState(std::move(state));
  return result;
}
ProteinSchemaMergeTailResult
protein_schema_finish_merge(const ProteinSchemaDefinitionState &source,
                            const ProteinSchemaDefinitionState &target,
                            uint32_t sv, uint32_t tv, bool compatible,
                            ProteinSchemaMergeTailOptions options) {
  using Issue = ProteinSchemaMergeTailIssue;
  ProteinSchemaMergeTailResult out{source, target, {}};
  out.compatible = compatible;
  out.version.status = ProteinSchemaVersionFinalStatus::unchanged;
  if (!compatible)
    return out;
  const bool self = source.same_identity(target);
  detail::require(
      !self || source.impl == target.impl,
      "Protein Schema merge tail has divergent snapshots of one scope");
  detail::require(
      options.profile == ProteinSchemaIterationProfile::reader_2017 ||
          options.profile == ProteinSchemaIterationProfile::reader_2026,
      "Protein Schema merge tail profile");
  auto version = target.finalize_version(sv, tv, true, options.comparison,
                                         options.definition);
  out.target = std::move(version.state);
  out.version = std::move(version.step);
  out.version_property = version.property;
  out.version_created = version.created;
  if (self)
    out.source = out.target;
  if (out.version.status == ProteinSchemaVersionFinalStatus::unresolved) {
    out.issue = Issue::version_unresolved;
    return out;
  }
  if (out.target.definition().status() !=
      ProteinSchemaDefinitionStatus::ready) {
    out.issue = Issue::target_iteration_unresolved;
    return out;
  }
  uint64_t count = 0;
  for (auto group : {ProteinSchemaPropertyContainer::ordinary,
                     ProteinSchemaPropertyContainer::iterator_data,
                     ProteinSchemaPropertyContainer::readonly_data})
    count += out.target.definition().container(group).size();
  detail::require(count <= options.max_target_properties,
                  "Protein Schema merge tail target property limit");
  for (auto row :
       out.target.definition().iteration({options.profile, false, false})) {
    const auto name = out.target.property(row).target().id;
    const auto found =
        options.profile == ProteinSchemaIterationProfile::reader_2017
            ? out.source.find(name)
            : out.source.find_actual(name);
    ++out.checked_target_properties;
    if (found.status == ProteinSchemaDefinitionLookupStatus::unresolved) {
      out.issue = Issue::source_lookup_unresolved;
      return out;
    }
    if (found.status != ProteinSchemaDefinitionLookupStatus::found) {
      out.first_target_only = row;
      out.source = out.source.with_merge_marker_byte(1);
      if (self)
        out.target = out.source;
      break;
    }
  }
  if (out.source.definition().status() !=
      ProteinSchemaDefinitionStatus::ready) {
    out.issue = Issue::aliases_unresolved;
    return out;
  }
  const auto before = out.target.alias_attempts().size();
  out.target = out.target.copy_aliases_from(out.source, options.aliases);
  out.alias_attempts = out.target.alias_attempts().size() - before;
  if (self)
    out.source = out.target;
  return out;
}

struct ProteinSchemaDefinitionGraph::Impl {
  struct Topology {
    std::vector<ProteinSchemaDefinitionGraphNode> nodes;
  };
  std::shared_ptr<const Topology> topology;
  std::map<Id, ProteinSchemaDefinitionState> updates;
  bool alias_parents = false;
  explicit Impl(std::shared_ptr<const Topology> value)
      : topology(std::move(value)) {}
};
ProteinSchemaDefinitionGraph::ProteinSchemaDefinitionGraph(
    std::shared_ptr<const Impl> value)
    : impl(std::move(value)) {}
ProteinSchemaDefinitionGraph::ProteinSchemaDefinitionGraph(
    std::span<const ProteinSchemaDefinitionGraphNode> nodes, uint32_t limit) {
  detail::require(nodes.size() <= limit && nodes.size() < uint64_t(none),
                  "Protein Schema definition graph node limit");
  std::unordered_set<const uint8_t *> identities;
  for (const auto &node : nodes)
    detail::require(
        node.state.impl &&
            identities.insert(node.state.impl->identity.get()).second,
        "Protein Schema graph requires one row per scope identity");
  auto topology = std::make_shared<Impl::Topology>();
  topology->nodes.assign(nodes.begin(), nodes.end());
  impl = std::make_shared<const Impl>(std::move(topology));
}
bool ProteinSchemaDefinitionGraph::same_identity(
    const ProteinSchemaDefinitionGraph &other) const noexcept {
  return impl && other.impl && impl->topology == other.impl->topology;
}
size_t ProteinSchemaDefinitionGraph::node_count() const noexcept {
  return impl->topology->nodes.size();
}
ProteinSchemaDefinitionRelation
ProteinSchemaDefinitionGraph::relation(Id node) const {
  detail::require(node < node_count(), "Protein Schema graph row out of range");
  return impl->topology->nodes[node].relation;
}
const ProteinSchemaDefinitionState &
ProteinSchemaDefinitionGraph::definition(Id node) const {
  detail::require(node < node_count(), "Protein Schema graph row out of range");
  if (const auto i = impl->updates.find(node); i != impl->updates.end())
    return i->second;
  return impl->topology->nodes[node].state;
}
ProteinSchemaDefinitionGraph ProteinSchemaDefinitionGraph::with_definition(
    Id node, const ProteinSchemaDefinitionState &state) const {
  const auto &current = definition(node);
  detail::require(current.same_identity(state),
                  "Protein Schema graph scope identity mismatch");
  if (current.impl == state.impl)
    return *this;
  auto result = std::make_shared<Impl>(*impl);
  result->updates.insert_or_assign(node, state);
  return ProteinSchemaDefinitionGraph(std::move(result));
}
ProteinSchemaReceiverSelection ProteinSchemaDefinitionGraph::select_receivers(
    Id target, std::span<const Id> entries,
    ProteinSchemaReceiverOptions options) const {
  using I = ProteinSchemaReceiverIssue;
  ProteinSchemaReceiverSelection result;
  if (target >= node_count()) {
    result.issue = I::invalid_target;
    result.issue_node = target;
    return result;
  }
  if (options.profile == ProteinSchemaReceiverProfile::local_target) {
    result.receivers.push_back(target);
    return result;
  }
  detail::require(entries.size() <= options.max_namespace_entries,
                  "Protein Schema namespace enumeration limit");
  // Immutable parent suffixes can be reused without materializing duplicate
  // chains or following an orphan cycle forever. Marks:
  // reaches/detached/active.
  std::unordered_map<Id, uint8_t> marks{{target, 1}};
  std::unordered_set<Id> selected{target};
  std::vector<Id> path;
  for (size_t ordinal = 0; ordinal < entries.size(); ++ordinal) {
    auto current = entries[ordinal];
    path.clear();
    bool reaches = false;
    while (current != none) {
      if (current >= node_count()) {
        result.issue = I::missing_node;
        result.issue_node = current;
        break;
      }
      if (const auto i = marks.find(current); i != marks.end()) {
        if (i->second == 3) {
          result.issue = I::parent_cycle;
          result.issue_node = current;
        } else
          reaches = i->second == 1;
        break;
      }
      marks.emplace(current, 3);
      path.push_back(current);
      const auto edge = relation(current);
      if (!edge.parent_known) {
        result.issue = I::unknown_parent;
        result.issue_node = current;
        break;
      }
      detail::require(result.parent_edges < options.max_parent_edges,
                      "Protein Schema parent edge limit");
      ++result.parent_edges;
      current = edge.parent;
    }
    if (result.issue != I::none) {
      result.namespace_ordinal = static_cast<Id>(ordinal);
      break;
    }
    for (const auto node : path) {
      marks[node] = reaches ? 1 : 2;
      if (reaches)
        selected.insert(node);
    }
  }
  result.receivers.assign(selected.begin(), selected.end());
  std::sort(result.receivers.begin(), result.receivers.end());
  if (result.issue != I::none)
    return result;
  if (!options.namespace_complete) {
    result.issue = I::namespace_unresolved;
    return result;
  }
  if (options.order == ProteinSchemaReceiverOrder::unsigned_keys &&
      result.receivers.size() > 1) {
    std::vector<std::pair<uint64_t, Id>> keyed;
    keyed.reserve(result.receivers.size());
    for (const auto node : result.receivers) {
      const auto key = relation(node).order_key;
      if (!key) {
        result.issue = I::missing_order_key;
        result.issue_node = node;
        return result;
      }
      keyed.emplace_back(*key, node);
    }
    std::sort(keyed.begin(), keyed.end());
    for (size_t i = 1; i < keyed.size(); ++i)
      if (keyed[i - 1].first == keyed[i].first) {
        result.issue = I::duplicate_order_key;
        result.issue_node = keyed[i].second;
        return result;
      }
    for (size_t i = 0; i < keyed.size(); ++i)
      result.receivers[i] = keyed[i].second;
  }
  return result;
}
ProteinSchemaDefinitionPropagationResult
ProteinSchemaDefinitionGraph::create_reflected_properties(
    Id target, std::span<const Id> entries,
    std::span<const ProteinSchemaPropertyState> sources,
    ProteinSchemaDefinitionPropagationOptions options) const {
  using I = ProteinSchemaPropagationIssue;
  using C = ProteinSchemaPropertyCopyIssue;
  ProteinSchemaDefinitionPropagationResult result{*this, {}, {}};
  detail::require(sources.size() <=
                      options.creation.definition.max_declarations,
                  "Protein Schema propagation source limit");
  if (sources.empty())
    return result;
  result.selection_evaluated = true;
  result.selection = select_receivers(target, entries, options.receivers);
  if (result.selection.issue != ProteinSchemaReceiverIssue::none) {
    result.issue = I::selection_unresolved;
    return result;
  }
  const uint64_t receivers = result.selection.receivers.size();
  uint64_t values = 0, bytes = 0;
  auto charge = [](uint64_t &used, uint64_t count, uint64_t limit,
                   const char *message) {
    detail::require(used <= limit && count <= limit - used, message);
    used += count;
  };
  for (const auto &source : sources) {
    result.copy_issue = schema_reflected_copy_issue(source);
    if (result.copy_issue != C::none) {
      result.issue = I::copy_unresolved;
      break;
    }
    detail::require(result.next_source < options.max_new_properties / receivers,
                    "Protein Schema propagation property limit");
    charge(values, schema_reflected_value_cost(source),
           options.max_new_cached_values / receivers,
           "Protein Schema propagation value limit");
    charge(bytes, source.target().id.size(),
           options.max_new_text_bytes / receivers,
           "Protein Schema propagation text limit");
    charge(bytes, source.label().size(), options.max_new_text_bytes / receivers,
           "Protein Schema propagation text limit");
    ++result.next_source;
  }
  result.new_cached_values = values * receivers;
  result.new_text_bytes = bytes * receivers;
  if (!result.next_source)
    return result;
  for (const auto receiver : result.selection.receivers) {
    charge(result.receiver_history,
           definition(receiver).definition().properties().size(),
           options.max_receiver_history,
           "Protein Schema propagation metadata history limit");
    charge(result.receiver_history, result.next_source,
           options.max_receiver_history,
           "Protein Schema propagation metadata history limit");
  }
  auto graph = std::make_shared<Impl>(*impl);
  std::vector<std::vector<ProteinSchemaReflectedCreation>> batches;
  batches.reserve(receivers);
  for (const auto receiver : result.selection.receivers) {
    auto batch = definition(receiver).create_reflected_properties(
        sources.first(result.next_source), options.creation);
    detail::require(batch.issue == C::none &&
                        batch.next_source == result.next_source,
                    "Protein Schema propagation copy preflight mismatch");
    graph->updates.insert_or_assign(receiver, std::move(batch.state));
    batches.push_back(std::move(batch.properties));
  }
  result.properties.reserve(result.next_source * receivers);
  for (size_t source = 0; source < result.next_source; ++source)
    for (size_t receiver = 0; receiver < batches.size(); ++receiver) {
      const auto &entry = batches[receiver][source];
      result.properties.push_back({source, result.selection.receivers[receiver],
                                   entry.property, entry.replaced,
                                   entry.container});
    }
  result.graph = ProteinSchemaDefinitionGraph(std::move(graph));
  return result;
}
ProteinSchemaComparisonResult protein_schema_compare_property_states(
    const ProteinSchemaPropertyState *a, const ProteinSchemaPropertyState *b,
    uint32_t skip_type, ProteinSchemaComparisonOptions options) {
  SchemaComparison compare{options};
  return schema_compare_property_states(a, b, skip_type, compare);
}
ProteinSchemaPropertyVersionResult protein_schema_apply_ordinary_version(
    const ProteinSchemaPropertyState &source,
    const ProteinSchemaPropertyState &target, uint32_t source_version,
    uint32_t target_version, ProteinSchemaComparisonOptions options) {
  SchemaComparison compare{options};
  return schema_apply_ordinary_version(source, target, source_version,
                                       target_version, compare);
}
ProteinSchemaComparisonResult
protein_schema_compare_base_properties(const ProteinSchemaPropertyState *target,
                                       const ProteinSchemaPropertyState *source,
                                       ProteinSchemaComparisonOptions options) {
  SchemaComparison compare{options};
  return schema_compare_base_properties(target, source, compare);
}
ProteinSchemaUriResult
protein_schema_apply_uri(const ProteinSchemaPropertyState &source,
                         const ProteinSchemaPropertyState &target,
                         ProteinSchemaUriContext context,
                         ProteinSchemaComparisonOptions options) {
  SchemaComparison compare{options};
  return schema_apply_uri(source, target, context, compare);
}
template <class ResolveVersions>
ProteinSchemaExistingPropertyResult schema_reconcile_existing_property(
    const ProteinSchemaPropertyState &input_source,
    const ProteinSchemaPropertyState &target,
    ProteinSchemaExistingPropertyContext context, SchemaComparison &compare,
    ResolveVersions resolve_versions) {
  using Status = ProteinSchemaExistingPropertyStatus;
  using Route = ProteinSchemaExistingPropertyRoute;
  using Issue = ProteinSchemaExistingPropertyIssue;
  ProteinSchemaExistingPropertyResult result{target};
  auto source = input_source;
  result.source = source;
  if (source.target().id == "SchemaVersion") {
    result.route = Route::schema_version_required;
    result.issue = Issue::schema_version_required;
    return result;
  }
  if (context.source_context) {
    result.source_repair =
        protein_schema_repair_source_flags(source, *context.source_context);
    if (result.source_repair->issue != ProteinSchemaFlagsRepairIssue::none) {
      result.route = Route::source_repair;
      result.issue = Issue::source_repair_unresolved;
      return result;
    }
    source = result.source_repair->source;
    result.source = source;
    const auto &raw = *context.source_context;
    context.uri_context.source_context_complete =
        raw.presence == ProteinSchemaReferencePresence::absent ||
        (raw.presence == ProteinSchemaReferencePresence::present &&
         raw.word_d0.has_value());
    context.uri_context.source_context_word =
        raw.presence == ProteinSchemaReferencePresence::present ? raw.word_d0
                                                                : std::nullopt;
  }
  if (!context.parent_lookup_complete) {
    result.route = Route::parent_check;
    result.issue = Issue::parent_lookup_unresolved;
    return result;
  }
  if (context.source_parent_property) {
    result.route = Route::parent_check;
    result.parent_comparison = schema_compare_property_states(
        &target, context.source_parent_property, 0, compare);
    if (result.parent_comparison->status == CompareStatus::unresolved) {
      result.issue = Issue::parent_comparison_unresolved;
      return result;
    }
    if (result.parent_comparison->status == CompareStatus::equal) {
      result.route = Route::parent_override;
      result.copy_attempted = true;
      auto copied = target.copied_from(source);
      result.copy_issue = copied.issue;
      if (!copied.state) {
        result.issue = Issue::parent_copy_unresolved;
        return result;
      }
      result.target = std::move(*copied.state);
      result.copied = true;
      result.status = Status::compatible;
      return result;
    }
  }
  if (source.target().id == "BaseSchema") {
    result.route = Route::base_schema;
    result.base_comparison =
        schema_compare_base_properties(&target, &source, compare);
    switch (result.base_comparison->status) {
    case CompareStatus::equal:
      result.status = Status::compatible;
      break;
    case CompareStatus::different:
      result.status = Status::incompatible;
      result.issue = Issue::base_comparison_different;
      break;
    case CompareStatus::unresolved:
      result.issue = Issue::base_comparison_unresolved;
      break;
    }
    return result;
  }
  if (target.target().value_type == 4) {
    result.route = Route::uri;
    result.uri = schema_apply_uri(source, target, context.uri_context, compare);
    result.target = result.uri->target;
    switch (result.uri->status) {
    case ProteinSchemaUriStatus::compatible:
      result.status = Status::compatible;
      break;
    case ProteinSchemaUriStatus::incompatible:
      result.status = Status::incompatible;
      result.issue = Issue::uri_incompatible;
      break;
    case ProteinSchemaUriStatus::unresolved:
      result.issue = Issue::uri_unresolved;
      break;
    }
    return result;
  }
  result.route = Route::ordinary_version;
  resolve_versions(context);
  if (!context.source_version || !context.target_version) {
    result.issue = Issue::unknown_version;
    return result;
  }
  result.version =
      schema_apply_ordinary_version(source, target, *context.source_version,
                                    *context.target_version, compare);
  result.target = result.version->target;
  result.copy_issue = result.version->copy_issue;
  result.copy_attempted = result.version->copy_attempted;
  result.copied = result.version->copied;
  switch (result.version->status) {
  case ProteinSchemaPropertyVersionStatus::compatible:
    result.status = Status::compatible;
    break;
  case ProteinSchemaPropertyVersionStatus::incompatible:
    result.status = Status::incompatible;
    result.issue = Issue::ordinary_version_incompatible;
    break;
  case ProteinSchemaPropertyVersionStatus::unresolved:
    result.issue = Issue::ordinary_version_unresolved;
    break;
  }
  return result;
}
ProteinSchemaExistingPropertyResult protein_schema_reconcile_existing_property(
    const ProteinSchemaPropertyState &source,
    const ProteinSchemaPropertyState &target,
    ProteinSchemaExistingPropertyContext context,
    ProteinSchemaComparisonOptions options) {
  SchemaComparison compare{options};
  return schema_reconcile_existing_property(source, target, context, compare,
                                            [](auto &) {});
}
namespace {
struct SchemaAliasNodeView {
  const ProteinSchemaAliasIndex *aliases;
  ProteinSchemaDefinitionStatus status;
};
template <class Read, class Parent> struct SchemaGraphAliases {
  using Issue = ProteinSchemaGraphAliasIssue;
  size_t count;
  bool dynamic;
  ProteinSchemaGraphAliasOptions limits;
  Read read;
  Parent parent;
  uint64_t edges = 0;
  // Query names borrow the owning graph/transaction or the calling function.
  // Alias tables do not change during the source loop; payload writes do not
  // invalidate these name bindings. No cache survives the composed operation.
  std::map<std::pair<Id, std::string_view>, ProteinSchemaGraphAliasLookup>
      cache;
  void edge() {
    detail::require(edges < limits.max_parent_edges,
                    "Protein Schema graph alias parent limit");
    ++edges;
  }
  ProteinSchemaGraphAliasLookup find(Id start, std::string_view name) {
    ProteinSchemaGraphAliasLookup out;
    if (start >= count) {
      out.issue = Issue::invalid_scope;
      out.issue_scope = start;
      return out;
    }
    const auto key = std::pair(start, name);
    if (auto i = cache.find(key); i != cache.end()) {
      out = i->second;
      out.parent_edges = 0;
      return out;
    }
    const auto before = edges;
    std::unordered_set<Id> seen;
    for (Id node = start; node != none;) {
      if (node >= count) {
        out.issue = Issue::missing_node;
        out.issue_scope = node;
        break;
      }
      if (!seen.insert(node).second) {
        out.issue = Issue::parent_cycle;
        out.issue_scope = node;
        break;
      }
      const auto view = read(node);
      if (view.status != ProteinSchemaDefinitionStatus::ready) {
        out.issue = Issue::scope_unresolved;
        out.issue_scope = node;
        break;
      }
      out.binding =
          dynamic ? view.aliases->find_local(name) : view.aliases->find(name);
      if (out.binding) {
        out.scope = dynamic ? node : none;
        break;
      }
      if (!dynamic)
        break;
      edge();
      const auto relation = parent(node);
      if (!relation.parent_known) {
        out.issue = Issue::unknown_parent;
        out.issue_scope = node;
        break;
      }
      node = relation.parent;
    }
    out.parent_edges = edges - before;
    if (out.issue == Issue::none && cache.size() < limits.max_cached_queries)
      cache.emplace(key, out);
    return out;
  }
  ProteinSchemaGraphAliasEnumeration enumerate(Id start) {
    ProteinSchemaGraphAliasEnumeration out;
    if (start >= count) {
      out.issue = Issue::invalid_scope;
      out.issue_scope = start;
      return out;
    }
    const auto before = edges;
    std::unordered_set<Id> seen;
    for (Id node = start; node != none;) {
      if (node >= count) {
        out.issue = Issue::missing_node;
        out.issue_scope = node;
        break;
      }
      if (!seen.insert(node).second) {
        out.issue = Issue::parent_cycle;
        out.issue_scope = node;
        break;
      }
      const auto view = read(node);
      if (view.status != ProteinSchemaDefinitionStatus::ready) {
        out.issue = Issue::scope_unresolved;
        out.issue_scope = node;
        break;
      }
      if (!dynamic) {
        for (const auto &a : view.aliases->enumerate(limits.max_aliases))
          out.aliases.push_back({none, a});
        break;
      }
      out.scopes.push_back(node);
      const auto local = view.aliases->local_aliases();
      detail::require(out.aliases.size() <= limits.max_aliases &&
                          local.size() <=
                              limits.max_aliases - out.aliases.size(),
                      "Protein Schema graph alias enumeration limit");
      for (const auto &a : local)
        out.aliases.push_back({node, a});
      edge();
      const auto relation = parent(node);
      if (!relation.parent_known) {
        out.issue = Issue::unknown_parent;
        out.issue_scope = node;
        break;
      }
      node = relation.parent;
    }
    out.parent_edges = edges - before;
    return out;
  }
  template <class State>
  ProteinSchemaGraphPropertyLookup property(Id node, std::string_view name,
                                            State state) {
    ProteinSchemaGraphPropertyLookup out;
    if (node >= count) {
      out.property.status = ProteinSchemaDefinitionLookupStatus::unresolved;
      out.alias.issue = Issue::invalid_scope;
      out.alias.issue_scope = node;
      return out;
    }
    const auto scope = state(node);
    out.property = scope.find_actual(name);
    if (out.property.status != ProteinSchemaDefinitionLookupStatus::missing)
      return out;
    out.alias_consulted = true;
    out.alias = find(node, name);
    if (out.alias.issue != Issue::none)
      out.property.status = ProteinSchemaDefinitionLookupStatus::unresolved;
    else if (out.alias.binding)
      out.property = scope.find_actual(out.alias.binding->target);
    return out;
  }
};
} // namespace

// Private builders never escape while mutable. Seeds address rows through their
// owning index, so append/reallocation does not turn a row ID into an address.
struct ProteinSchemaReconciliationAccess {
  using Scope = ProteinSchemaDefinitionState;
  using Metadata = ProteinSchemaDefinitionIndex::Impl;
  static SchemaAliasNodeView alias_view(const Scope &s) {
    return {&s.impl->index.impl->visible_aliases, s.definition().status()};
  }
  static Scope with_aliases(const Scope &s, ProteinSchemaAliasIndex aliases,
                            uint64_t limit) {
    detail::require(s.definition().properties().size() <= limit,
                    "Protein Schema alias transfer metadata history limit");
    auto m = std::make_shared<Metadata>(*s.impl->index.impl);
    m->visible_aliases = std::move(aliases);
    auto out = std::make_shared<Scope::Impl>(*s.impl);
    out->index = ProteinSchemaDefinitionIndex(std::move(m));
    return Scope(std::move(out));
  }
  struct RegisteredAliases {
    ProteinSchemaAliasIndex aliases;
    size_t next = 0;
    ProteinSchemaGraphAliasLookup blocked;
    std::vector<ProteinSchemaScopedAlias> inputs = {};
    std::optional<ProteinSchemaScopedAlias> blocked_input = std::nullopt;
  };
  template <class Lookup, class Owner>
  static RegisteredAliases
  register_aliases(const ProteinSchemaAliasIndex &destination, Id target,
                   const ProteinSchemaGraphAliasEnumeration &source,
                   ProteinSchemaAliasOptions options, Lookup lookup,
                   Owner owner) {
    using Impl = ProteinSchemaAliasIndex::Impl;
    RegisteredAliases out{destination, 0, {}};
    detail::require(source.aliases.size() <= options.max_enumerated_aliases,
                    "Protein Schema graph alias source limit");
    std::shared_ptr<Impl::Local> local;
    std::shared_ptr<Impl> result;
    std::map<std::string_view, ProteinSchemaAliasBinding, std::less<>> targets;
    std::set<const Impl *> retained;
    auto retain = [&](Id node) {
      if (node == target)
        return; // copied local document/import owners suffice
      const auto p = owner(node).impl;
      // Distinct scopes can share exactly this immutable local table. Its
      // document/import owners are already in the copied Local; retaining a
      // whole older index would add a needless ownership chain.
      if (p->local == destination.impl->local)
        return;
      if (!retained.insert(p.get()).second)
        return;
      local->registration_depth = std::max(local->registration_depth, p->depth);
      detail::require(local->registration_depth < options.max_scope_depth,
                      "Protein Schema graph alias ownership depth limit");
      result->depth = std::max(result->depth, local->registration_depth + 1);
      local->imported_sources.push_back(p);
    };
    size_t front = 0;
    bool ancestor = false;
    for (auto node : source.scopes) {
      if (node == target) {
        ancestor = true;
        break;
      }
      front += owner(node).local_aliases().size();
    }
    const auto original_local = destination.local_aliases().size();
    for (size_t ordinal = 0; ordinal < source.aliases.size(); ++ordinal) {
      auto input = source.aliases[ordinal];
      if (ancestor && local && ordinal >= front) {
        if (ordinal - front < local->accepted.size())
          input = {target, local->accepted[ordinal - front]};
        else
          input =
              source
                  .aliases[ordinal - (local->accepted.size() - original_local)];
      }
      const auto &a = input.alias;
      ProteinSchemaAliasEntry entry;
      entry.document = a.binding.document;
      entry.source = a.binding.source;
      entry.name = a.name;
      entry.target = a.binding.target;
      const auto &known = local ? *local : *destination.impl->local;
      std::optional<ProteinSchemaAliasBinding> conflict;
      if (const auto i = known.accepted_lookup.find(a.name);
          i != known.accepted_lookup.end())
        conflict = known.accepted[i->second].binding;
      ProteinSchemaGraphAliasLookup visible;
      if (!conflict) {
        visible = lookup(a.name);
        if (visible.issue != ProteinSchemaGraphAliasIssue::none) {
          out.blocked = visible;
          out.blocked_input = input;
          break;
        }
        conflict = visible.binding;
      }
      if (!local) {
        local = std::make_shared<Impl::Local>(*destination.impl->local);
        result =
            std::make_shared<Impl>(local, destination.impl->parent, options);
        for (const auto &p : local->imported_sources)
          retained.insert(p.get());
        // A later indexed rebind still needs rejection provenance from the
        // previously bound indexed parent, independently of graph parents.
        if (destination.impl->parent) {
          local->registration_depth = std::max(local->registration_depth,
                                               destination.impl->parent->depth);
          if (retained.insert(destination.impl->parent.get()).second)
            local->imported_sources.push_back(destination.impl->parent);
          detail::require(local->registration_depth < options.max_scope_depth,
                          "Protein Schema graph alias ownership depth limit");
          result->depth =
              std::max(result->depth, local->registration_depth + 1);
        }
        for (const auto &[name, binding] : local->accepted)
          targets.emplace(binding.target, binding);
      }
      detail::require(local->entries.size() < options.max_aliases,
                      "Protein Schema graph alias local history limit");
      retain(input.scope);
      if (conflict) {
        entry.status = ProteinSchemaAliasStatus::duplicate_name;
        entry.conflict = conflict;
        if (visible.binding)
          retain(visible.scope);
      } else if (const auto i = targets.find(a.binding.target);
                 i != targets.end()) {
        entry.status = ProteinSchemaAliasStatus::duplicate_local_target;
        entry.conflict = i->second;
      } else {
        entry.status = ProteinSchemaAliasStatus::accepted;
        if (!result->visible.contains(a.name))
          detail::require(result->visible.size() < options.max_visible_aliases,
                          "Protein Schema graph alias indexed storage limit");
        result->visible.insert_or_assign(a.name, a.binding);
        local->accepted_lookup.emplace(a.name, local->accepted.size());
        local->accepted.push_back(a);
        targets.emplace(a.binding.target, a.binding);
      }
      local->entries.push_back(entry);
      out.inputs.push_back(input);
      ++out.next;
    }
    if (result)
      out.aliases = ProteinSchemaAliasIndex(std::move(result));
    return out;
  }
  static bool same_property_snapshot(const ProteinSchemaPropertyState &a,
                                     const ProteinSchemaPropertyState &b) {
    return a.impl == b.impl;
  }
  static void charge(uint64_t &used, uint64_t amount, uint64_t limit,
                     const char *message) {
    detail::require(used <= limit && amount <= limit - used, message);
    used += amount;
  }
  struct Builder {
    Scope input;
    std::shared_ptr<Scope::Impl> state;
    std::shared_ptr<Metadata> metadata;
    ProteinSchemaReconciliationResult &out;
    const ProteinSchemaReconciliationOptions &options;
    size_t reflected = 0;
    bool order_dirty = false;
    Builder(const Scope &s, ProteinSchemaReconciliationResult &r,
            const ProteinSchemaReconciliationOptions &o)
        : input(s), out(r), options(o) {}
    Scope current() const { return state ? Scope(state) : input; }
    Scope::Impl &edit() {
      if (!state) {
        charge(out.state_history, input.impl->updates.size(),
               options.max_state_history,
               "Protein Schema reconciliation state history limit");
        state = std::make_shared<Scope::Impl>(*input.impl);
      }
      return *state;
    }
    Metadata &edit_metadata() {
      if (!metadata) {
        charge(out.metadata_history, input.definition().properties().size(),
               options.max_metadata_history,
               "Protein Schema reconciliation metadata history limit");
        metadata = std::make_shared<Metadata>(*input.impl->index.impl);
        edit().index = ProteinSchemaDefinitionIndex(metadata);
      }
      return *metadata;
    }
    void update(Id row, const ProteinSchemaPropertyState &value) {
      auto &s = edit();
      if (!s.updates.contains(row))
        charge(out.state_history, 1, options.max_state_history,
               "Protein Schema reconciliation state history limit");
      s.updates.insert_or_assign(row, value);
    }
    void growth(uint64_t values, uint64_t bytes) {
      charge(out.new_properties, 1, options.propagation.max_new_properties,
             "Protein Schema reconciliation new property limit");
      charge(out.new_cached_values, values,
             options.propagation.max_new_cached_values,
             "Protein Schema reconciliation new value limit");
      charge(out.new_text_bytes, bytes, options.propagation.max_new_text_bytes,
             "Protein Schema reconciliation new text limit");
      charge(out.metadata_history, 1, options.max_metadata_history,
             "Protein Schema reconciliation metadata history limit");
    }
    Id reflect(const ProteinSchemaPropertyState &source) {
      if (reflected == 0)
        charge(out.reflected_receiver_history,
               current().definition().properties().size(),
               options.propagation.max_receiver_history,
               "Protein Schema reconciliation receiver history limit");
      charge(out.reflected_receiver_history, 1,
             options.propagation.max_receiver_history,
             "Protein Schema reconciliation receiver history limit");
      detail::require(
          reflected < options.propagation.creation.definition.max_declarations,
          "Protein Schema reconciliation receiver declaration limit");
      ++reflected;
      auto &m = edit_metadata();
      const auto name_bytes = source.target().id.size();
      detail::require(source.label().size() <=
                          std::numeric_limits<uint64_t>::max() - name_bytes,
                      "Protein Schema reconciliation text size");
      growth(schema_reflected_value_cost(source),
             name_bytes + source.label().size());
      const auto row = m.insert_reflected(source, options.propagation.creation);
      order_dirty = true;
      ProteinSchemaPropertyState created(current().definition(), row);
      const auto copied = created.copied_from(source);
      detail::require(copied.state.has_value(),
                      "Protein Schema reconciliation copy preflight mismatch");
      update(row, *copied.state);
      return row;
    }
    ProteinSchemaVersionRead
    version(SchemaComparison &compare,
            const ProteinSchemaDefinitionLookup *resolved = nullptr) const {
      const auto scope = current();
      const auto found = resolved ? *resolved : scope.find("SchemaVersion");
      const auto value =
          found.status == ProteinSchemaDefinitionLookupStatus::found
              ? std::optional(scope.property(found.property))
              : std::nullopt;
      return schema_read_version(
          value ? &*value : nullptr,
          found.status != ProteinSchemaDefinitionLookupStatus::unresolved,
          compare);
    }
    ProteinSchemaScopedVersionResult
    final_version(uint32_t sv, uint32_t tv, SchemaComparison &compare,
                  const ProteinSchemaDefinitionLookup *resolved = nullptr) {
      ProteinSchemaScopedVersionResult result{current(), {}};
      if (sv <= tv) {
        result.step.status = ProteinSchemaVersionFinalStatus::unchanged;
        return result;
      }
      const auto scope = current();
      const auto found = resolved ? *resolved : scope.find("SchemaVersion");
      const auto value =
          found.status == ProteinSchemaDefinitionLookupStatus::found
              ? std::optional(scope.property(found.property))
              : std::nullopt;
      result.property = found.property;
      result.step = schema_finalize_version(
          value ? &*value : nullptr, sv, tv, true,
          found.status != ProteinSchemaDefinitionLookupStatus::unresolved,
          compare);
      if (value && result.step.written)
        update(found.property, *result.step.target);
      if (result.step.status ==
          ProteinSchemaVersionFinalStatus::creation_required) {
        auto &m = edit_metadata();
        growth(1, 0);
        result.property =
            m.insert_version(sv, options.propagation.creation.definition);
        order_dirty = true;
        ProteinSchemaPropertyState created(current().definition(),
                                           result.property);
        update(result.property, created);
        result.step.target = created;
        result.step.status = ProteinSchemaVersionFinalStatus::finished;
        result.step.written = true;
        result.created = true;
      }
      result.state = current();
      return result;
    }
    void marker() { edit().merge_marker = 1; }
    void copy_aliases(const Builder &source) {
      const auto source_scope = source.current();
      const auto &from = source_scope.impl->index.impl->visible_aliases;
      // Test the normalized sequence without copying a metadata index on no-op.
      if (from.enumerate(options.aliases.max_enumerated_aliases).empty())
        return;
      auto &m = edit_metadata();
      m.visible_aliases =
          m.visible_aliases.copy_aliases_from(from, options.aliases);
    }
    void order() {
      if (metadata && order_dirty) {
        metadata->finish();
        order_dirty = false;
      }
    }
    Scope freeze() {
      order();
      return current();
    }
  };
};

bool ProteinSchemaDefinitionGraph::aliases_follow_parents() const noexcept {
  return impl->alias_parents;
}
ProteinSchemaDefinitionGraph
ProteinSchemaDefinitionGraph::with_alias_parents(bool enabled) const {
  if (enabled == impl->alias_parents)
    return *this;
  auto graph = std::make_shared<Impl>(*impl);
  graph->alias_parents = enabled;
  return ProteinSchemaDefinitionGraph(std::move(graph));
}
ProteinSchemaGraphAliasLookup ProteinSchemaDefinitionGraph::find_alias(
    Id node, std::string_view name,
    ProteinSchemaGraphAliasOptions options) const {
  auto read = [&](Id id) {
    return ProteinSchemaReconciliationAccess::alias_view(definition(id));
  };
  auto parent = [&](Id id) { return relation(id); };
  SchemaGraphAliases runtime{
      node_count(), impl->alias_parents, options, read, parent, 0, {}};
  return runtime.find(node, name);
}
ProteinSchemaGraphPropertyLookup ProteinSchemaDefinitionGraph::find(
    Id node, std::string_view name,
    ProteinSchemaGraphAliasOptions options) const {
  auto state = [&](Id id) { return definition(id); };
  auto read = [&](Id id) {
    return ProteinSchemaReconciliationAccess::alias_view(definition(id));
  };
  auto parent = [&](Id id) { return relation(id); };
  SchemaGraphAliases runtime{
      node_count(), impl->alias_parents, options, read, parent, 0, {}};
  return runtime.property(node, name, state);
}
ProteinSchemaGraphAliasEnumeration
ProteinSchemaDefinitionGraph::enumerate_aliases(
    Id node, ProteinSchemaGraphAliasOptions options) const {
  auto read = [&](Id id) {
    return ProteinSchemaReconciliationAccess::alias_view(definition(id));
  };
  auto parent = [&](Id id) { return relation(id); };
  SchemaGraphAliases runtime{
      node_count(), impl->alias_parents, options, read, parent, 0, {}};
  return runtime.enumerate(node);
}
ProteinSchemaGraphAliasTransferResult
ProteinSchemaDefinitionGraph::copy_aliases(
    Id source, Id target,
    ProteinSchemaGraphAliasTransferOptions options) const {
  using A = ProteinSchemaReconciliationAccess;
  ProteinSchemaGraphAliasTransferResult out{*this, {}, {}};
  if (source >= node_count() || target >= node_count()) {
    out.issue = ProteinSchemaGraphAliasIssue::invalid_scope;
    out.blocked.issue = out.issue;
    out.blocked.issue_scope = source >= node_count() ? source : target;
    return out;
  }
  auto read = [&](Id id) { return A::alias_view(definition(id)); };
  auto parent = [&](Id id) { return relation(id); };
  SchemaGraphAliases runtime{
      node_count(), impl->alias_parents, options.graph, read, parent, 0, {}};
  out.source = runtime.enumerate(source);
  out.parent_edges = runtime.edges;
  if (out.source.issue != ProteinSchemaGraphAliasIssue::none) {
    out.issue = out.source.issue;
    return out;
  }
  if (out.source.aliases.empty())
    return out;
  if (definition(target).definition().status() !=
      ProteinSchemaDefinitionStatus::ready) {
    out.issue = out.blocked.issue =
        ProteinSchemaGraphAliasIssue::scope_unresolved;
    out.blocked.issue_scope = target;
    out.blocked_input = out.source.aliases.front();
    return out;
  }
  detail::require(definition(target).definition().properties().size() <=
                      options.max_metadata_history,
                  "Protein Schema alias transfer metadata history limit");
  if (!impl->alias_parents) {
    const auto changed = definition(target).copy_aliases_from(
        definition(source), options.aliases);
    out.graph = with_definition(target, changed);
    out.next_alias = out.source.aliases.size();
    out.inputs = out.source.aliases;
  } else {
    const auto changed = A::register_aliases(
        *read(target).aliases, target, out.source, options.aliases,
        [&](std::string_view name) { return runtime.find(target, name); },
        [&](Id id) -> const ProteinSchemaAliasIndex & {
          return *read(id).aliases;
        });
    out.next_alias = changed.next;
    out.blocked = changed.blocked;
    out.inputs = changed.inputs;
    out.blocked_input = changed.blocked_input;
    out.issue = changed.blocked.issue;
    if (out.next_alias)
      out.graph = with_definition(
          target, A::with_aliases(definition(target), changed.aliases,
                                  options.max_metadata_history));
  }
  if (out.next_alias)
    out.metadata_history = definition(target).definition().properties().size();
  out.parent_edges = runtime.edges;
  return out;
}

ProteinSchemaReconciliationResult
ProteinSchemaDefinitionGraph::reconcile_definition(
    Id source_id, Id target_id, std::span<const Id> enumeration,
    const ProteinSchemaReconciliationOptions &options) const {
  using A = ProteinSchemaReconciliationAccess;
  using B = A::Builder;
  using Issue = ProteinSchemaReconciliationIssue;
  using Status = ProteinSchemaReconciliationStatus;
  using Lookup = ProteinSchemaDefinitionLookupStatus;
  ProteinSchemaReconciliationResult out{
      *this, Status::unresolved, Issue::none, std::nullopt, std::nullopt, {},
      {},    std::nullopt,       std::nullopt};
  SchemaComparison compare{options.comparison};
  std::map<Id, std::unique_ptr<B>> scopes;
  auto builder = [&](Id id) -> B & {
    auto i = scopes.find(id);
    if (i == scopes.end())
      i = scopes.emplace(id, std::make_unique<B>(definition(id), out, options))
              .first;
    return *i->second;
  };
  auto current = [&](Id id) { return builder(id).current(); };
  auto read_alias = [&](Id id) { return A::alias_view(current(id)); };
  auto alias_parent = [&](Id id) { return relation(id); };
  SchemaGraphAliases aliases{node_count(),
                             impl->alias_parents,
                             options.graph_aliases,
                             read_alias,
                             alias_parent,
                             0,
                             {}};
  auto interface_lookup = [&](Id id, std::string_view name) {
    if (!impl->alias_parents)
      return current(id).find(name);
    const auto found = aliases.property(id, name, current);
    if (found.alias.issue != ProteinSchemaGraphAliasIssue::none) {
      out.alias_issue = found.alias.issue;
      out.alias_issue_scope = found.alias.issue_scope;
    }
    return found.property;
  };
  auto read_version = [&](Id id) {
    const auto found = interface_lookup(id, "SchemaVersion");
    return builder(id).version(compare, &found);
  };
  auto finish = [&]() {
    out.alias_parent_edges = aliases.edges;
    out.comparison_values = compare.values;
    out.comparison_text_bytes = compare.bytes;
    std::shared_ptr<Impl> graph;
    for (auto &[id, scope] : scopes)
      if (scope->state) {
        if (!graph)
          graph = std::make_shared<Impl>(*impl);
        graph->updates.insert_or_assign(id, scope->freeze());
      }
    if (graph)
      out.graph = ProteinSchemaDefinitionGraph(std::move(graph));
    return std::move(out);
  };
  auto unresolved = [&](Issue issue) {
    out.issue = issue;
    out.status = Status::unresolved;
    return finish();
  };
  if (source_id >= node_count())
    return unresolved(Issue::invalid_source);
  if (target_id >= node_count())
    return unresolved(Issue::invalid_target);
  detail::require(
      options.profile == ProteinSchemaIterationProfile::reader_2017 ||
          options.profile == ProteinSchemaIterationProfile::reader_2026,
      "Protein Schema reconciliation profile");
  auto &source = builder(source_id);
  auto &target = builder(target_id);
  if (source.input.definition().status() !=
          ProteinSchemaDefinitionStatus::ready ||
      target.input.definition().status() !=
          ProteinSchemaDefinitionStatus::ready)
    return unresolved(Issue::uid_unresolved);
  if (compare
          .text(source.input.definition().uid(),
                target.input.definition().uid(), 0)
          .status != CompareStatus::equal) {
    out.status = Status::incompatible;
    out.issue = Issue::uid_mismatch;
    return finish();
  }
  // Native reads target first, then source. These are the originals for the
  // final gate, not a cache for subsequent ordinary-property version reads.
  out.target_version = read_version(target_id).value;
  out.source_version = read_version(source_id).value;
  if (!out.target_version || !out.source_version)
    return unresolved(Issue::initial_version_unresolved);
  uint64_t source_count = 0;
  for (auto group : {ProteinSchemaPropertyContainer::ordinary,
                     ProteinSchemaPropertyContainer::iterator_data,
                     ProteinSchemaPropertyContainer::readonly_data})
    source_count += source.input.definition().container(group).size();
  detail::require(source_count <= options.max_source_properties,
                  "Protein Schema reconciliation source property limit");
  const auto rows =
      source.input.definition().iteration({options.profile, false, false});
  const bool old =
      options.profile == ProteinSchemaIterationProfile::reader_2017;
  auto lookup = [&](Id id, std::string_view name) {
    return old ? interface_lookup(id, name) : current(id).find_actual(name);
  };
  for (size_t ordinal = 0; ordinal < rows.size(); ++ordinal) {
    detail::require(out.created.size() <= options.max_events &&
                        out.events.size() <
                            options.max_events - out.created.size(),
                    "Protein Schema reconciliation event limit");
    const auto row = rows[ordinal];
    auto value = source.current().property(row);
    ProteinSchemaReconciliationEvent event;
    event.source_property = row;
    if (value.target().id == "SchemaVersion") {
      event.route = ProteinSchemaReconciliationRoute::schema_version_skip;
      out.events.push_back(std::move(event));
      out.next_source = ordinal + 1;
      continue;
    }
    const auto repair =
        protein_schema_repair_source_flags(value, options.source_context);
    event.source_flags_written = repair.written;
    if (repair.written)
      source.update(row, repair.source);
    if (repair.issue != ProteinSchemaFlagsRepairIssue::none) {
      out.events.push_back(std::move(event));
      return unresolved(Issue::source_repair_unresolved);
    }
    value = repair.source;
    const auto found = lookup(target_id, value.target().id);
    event.target_property = found.property;
    if (found.status == Lookup::unresolved) {
      out.events.push_back(std::move(event));
      return unresolved(Issue::target_lookup_unresolved);
    }
    if (found.status == Lookup::found) {
      ProteinSchemaExistingPropertyContext context;
      std::optional<ProteinSchemaPropertyState> parent_value;
      const auto parent = relation(source_id);
      context.parent_lookup_complete = parent.parent_known;
      if (parent.parent_known && parent.parent != none) {
        if (parent.parent >= node_count())
          context.parent_lookup_complete = false;
        else {
          auto &p = builder(parent.parent);
          const auto f = lookup(parent.parent, value.target().id);
          context.parent_lookup_complete = f.status != Lookup::unresolved;
          if (f.status == Lookup::found)
            parent_value = p.current().property(f.property);
        }
      }
      context.source_parent_property = parent_value ? &*parent_value : nullptr;
      const auto &raw = options.source_context;
      context.uri_context.source_context_complete =
          raw.presence == ProteinSchemaReferencePresence::absent ||
          (raw.presence == ProteinSchemaReferencePresence::present &&
           raw.word_d0.has_value());
      context.uri_context.source_context_word =
          raw.presence == ProteinSchemaReferencePresence::present
              ? raw.word_d0
              : std::nullopt;
      const auto current_target = target.current().property(found.property);
      auto step = schema_reconcile_existing_property(
          value, current_target, context, compare, [&](auto &c) {
            c.target_version = read_version(target_id).value;
            c.source_version = read_version(source_id).value;
          });
      if (!A::same_property_snapshot(current_target, step.target))
        target.update(found.property, step.target);
      const auto status = step.status;
      event.existing = std::move(step);
      out.events.push_back(std::move(event));
      if (status == ProteinSchemaExistingPropertyStatus::unresolved)
        return unresolved(context.parent_lookup_complete
                              ? Issue::existing_unresolved
                              : Issue::source_parent_unresolved);
      out.next_source = ordinal + 1;
      if (status == ProteinSchemaExistingPropertyStatus::incompatible) {
        out.status = Status::incompatible;
        return finish();
      }
    } else {
      event.route = ProteinSchemaReconciliationRoute::reflected;
      if (!out.selection) {
        auto receiver = options.propagation.receivers;
        receiver.profile =
            old ? ProteinSchemaReceiverProfile::local_target
                : ProteinSchemaReceiverProfile::namespace_ancestors;
        out.selection = select_receivers(target_id, enumeration, receiver);
      }
      if (out.selection->issue != ProteinSchemaReceiverIssue::none) {
        out.events.push_back(std::move(event));
        return unresolved(Issue::receiver_unresolved);
      }
      bool copy_checked = false;
      for (const auto receiver : out.selection->receivers) {
        // Prior receivers complete before native insertion can replace the
        // source's mapped object. That insertion requests destruction/free,
        // then the native loop reads the old raw source pointer again. Preserve
        // the proven prefix; do not invent ownership, a restart or a ready
        // half-initialized source replacement.
        if (receiver == source_id) {
          out.blocked_receiver = receiver;
          out.events.push_back(std::move(event));
          return unresolved(Issue::source_receiver_overlap);
        }
        if (!copy_checked) {
          out.copy_issue = schema_reflected_copy_issue(value);
          if (out.copy_issue != ProteinSchemaPropertyCopyIssue::none) {
            out.events.push_back(std::move(event));
            return unresolved(Issue::copy_unresolved);
          }
          copy_checked = true;
        }
        detail::require(out.events.size() < options.max_events &&
                            out.created.size() <
                                options.max_events - out.events.size() - 1,
                        "Protein Schema reconciliation creation event limit");
        auto &scope = builder(receiver);
        const auto generated = scope.reflect(value);
        const auto current_scope = scope.current();
        const auto &metadata =
            current_scope.definition().properties()[generated];
        out.created.push_back({ordinal, receiver, generated, metadata.replaced,
                               metadata.container});
        if (receiver == target_id)
          event.target_property = generated;
      }
      out.events.push_back(std::move(event));
      out.next_source = ordinal + 1;
    }
  }
  out.tail.emplace(
      ProteinSchemaMergeTailResult{source.current(), target.current(), {}});
  auto &tail = *out.tail;
  std::optional<ProteinSchemaDefinitionLookup> final_lookup;
  if (*out.source_version > *out.target_version)
    final_lookup = interface_lookup(target_id, "SchemaVersion");
  auto version =
      target.final_version(*out.source_version, *out.target_version, compare,
                           final_lookup ? &*final_lookup : nullptr);
  tail.version = std::move(version.step);
  tail.version_property = version.property;
  tail.version_created = version.created;
  auto tail_unresolved = [&](ProteinSchemaMergeTailIssue issue) {
    tail.issue = issue;
    tail.source = source.current();
    tail.target = target.current();
    return unresolved(Issue::tail_unresolved);
  };
  if (tail.version.status == ProteinSchemaVersionFinalStatus::unresolved)
    return tail_unresolved(ProteinSchemaMergeTailIssue::version_unresolved);
  target.order();
  uint64_t target_count = 0;
  for (auto group : {ProteinSchemaPropertyContainer::ordinary,
                     ProteinSchemaPropertyContainer::iterator_data,
                     ProteinSchemaPropertyContainer::readonly_data})
    target_count += target.current().definition().container(group).size();
  detail::require(target_count <= options.max_target_properties,
                  "Protein Schema reconciliation target property limit");
  for (auto row : target.current().definition().iteration(
           {options.profile, false, false})) {
    const auto f =
        lookup(source_id, target.current().property(row).target().id);
    ++tail.checked_target_properties;
    if (f.status == Lookup::unresolved)
      return tail_unresolved(
          ProteinSchemaMergeTailIssue::source_lookup_unresolved);
    if (f.status != Lookup::found) {
      tail.first_target_only = row;
      source.marker();
      break;
    }
  }
  const auto before = target.current().alias_attempts().size();
  if (!impl->alias_parents) {
    const auto snapshot = source.current().definition().enumerate_aliases(
        options.aliases.max_enumerated_aliases);
    target.copy_aliases(source);
    for (const auto &a : snapshot)
      out.alias_inputs.push_back({none, a});
  } else {
    const auto enumeration = aliases.enumerate(source_id);
    if (enumeration.issue != ProteinSchemaGraphAliasIssue::none) {
      out.alias_issue = enumeration.issue;
      out.alias_issue_scope = enumeration.issue_scope;
      return tail_unresolved(ProteinSchemaMergeTailIssue::aliases_unresolved);
    }
    const auto copied = A::register_aliases(
        *read_alias(target_id).aliases, target_id, enumeration, options.aliases,
        [&](std::string_view name) { return aliases.find(target_id, name); },
        [&](Id id) -> const ProteinSchemaAliasIndex & {
          return *read_alias(id).aliases;
        });
    if (copied.next)
      target.edit_metadata().visible_aliases = copied.aliases;
    out.alias_inputs = copied.inputs;
    out.alias_blocked_input = copied.blocked_input;
    if (copied.blocked.issue != ProteinSchemaGraphAliasIssue::none) {
      tail.alias_attempts = copied.next;
      out.alias_issue = copied.blocked.issue;
      out.alias_issue_scope = copied.blocked.issue_scope;
      return tail_unresolved(ProteinSchemaMergeTailIssue::aliases_unresolved);
    }
  }
  tail.alias_attempts = target.current().alias_attempts().size() - before;
  tail.source = source.current();
  tail.target = target.current();
  out.status = Status::compatible;
  return finish();
}

} // namespace nwd
