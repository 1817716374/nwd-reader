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
struct ProteinSchemaAliasIndex::Impl {
  struct Local {
    ProteinSchemaDocument document;
    std::vector<ProteinSchemaAliasEntry> entries;
    std::vector<std::pair<std::string_view, ProteinSchemaAliasBinding>>
        accepted;
    // Conflict pointers retain their original registration scope, even if a
    // later binding replaces the parent used for lookup.
    std::vector<std::pair<Id, std::shared_ptr<const Impl>>>
        registration_parents;
    uint32_t registration_depth = 0;
    explicit Local(const ProteinSchemaDocument &doc) : document(doc) {}
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
        ProteinSchemaAliasBinding binding{&state->document, source, *fields.id};
        visible.emplace(*fields.alias, binding);
        targets.emplace(*fields.id, binding);
        state->accepted.emplace_back(*fields.alias, binding);
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
  return impl->local->document;
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
std::optional<ProteinSchemaAliasBinding>
ProteinSchemaAliasIndex::find_before(Id source, std::string_view name) const {
  const auto &local = *impl->local;
  if (auto binding = find(name); binding &&
                                 binding->document == &local.document &&
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
bool schema_numeric_gate_known(const ProteinSchemaDocument &doc,
                               const ProteinSchemaEntryFields &fields,
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
    const auto raw = doc.constraint_fields(value.constraint.source);
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
          known = schema_numeric_gate_known(owned, fields, value);
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
} // namespace nwd
