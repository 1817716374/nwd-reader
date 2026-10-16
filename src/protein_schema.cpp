#include "internal.hpp"
#include "expat/nwd_expat_prefix.h"
#define XML_STATIC 1
#include "expat/lib/expat.h"
#include <exception>
#include <limits>
#include <charconv>
#include <map>
#include <cmath>

namespace nwd {
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
  static constexpr std::string_view units[]{
      "mm",   "millimeter", "millimetre", "millimeters", "millimetres",
      "cm",   "centimeter", "centimetre", "centimeters", "centimetres",
      "dm",   "decimeter",  "decimetre",  "decimeters",  "decimetres",
      "m",    "meter",      "metre",      "meters",      "metres",
      "km",   "kilometer",  "kilometre",  "kilometers",  "kilometres",
      "in",   "inch",       "inches",     "ft",          "foot",
      "feet", "yd",         "yard",       "yards",       "mi",
      "mile", "miles"};
  const auto unit = fields.unit.value_or(std::string_view{});
  for (auto known : units) {
    if (unit == known) {
      out.flags |= 0x400;
      break;
    }
  }
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
  const auto raw = constraint_fields(out.source);
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
  out.status = ProteinSchemaNumericStatus::ready;
  auto prepared = defaults(entry, options);
  bool uncertain = false;
  if (integer || real) {
    out.constraint = resolved_constraint(entry);
    auto bounds = out.constraint.range;
    if (out.constraint.status == ProteinSchemaConstraintStatus::unresolved) {
      const auto raw = constraint_fields(out.constraint.source);
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
} // namespace nwd
