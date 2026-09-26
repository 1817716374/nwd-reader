#include "internal.hpp"
#include <map>
namespace nwd {
namespace {
std::string_view namespace_of(std::string_view chunk) {
  auto split = chunk.rfind('\\');
  return split == chunk.npos ? std::string_view{} : chunk.substr(0, split);
}
template <size_t N>
std::string_view enum_name(const char *const (&names)[N],
                           int32_t value) noexcept {
  return value >= 0 && static_cast<size_t>(value) < N ? names[value] : "";
}
} // namespace
std::string_view animation_enum_name(AnimationEnum domain,
                                     int32_t value) noexcept {
  static constexpr const char *start[]{"start", "end", "current_position",
                                       "specified_time"};
  static constexpr const char *end[]{"start", "end", "specified_time"};
  static constexpr const char *stop[]{"default_position", "current_position"};
  static constexpr const char *modifier[]{"set_equal_to", "increment_by",
                                          "decrement_by"};
  static constexpr const char *comparison[]{"equal_to",
                                            "not_equal_to",
                                            "greater_than",
                                            "less_than",
                                            "greater_than_or_equal_to",
                                            "less_than_or_equal_to"};
  static constexpr const char *timer[]{"once_after", "continuous"};
  static constexpr const char *transition[]{"starting", "ending"};
  static constexpr const char *key[]{"key_up", "key_down", "key_pressed"};
  static constexpr const char *hotspot[]{"entering", "leaving", "in_range"};
  static constexpr const char *shape[]{"sphere", "sphere_on_selection"};
  switch (domain) {
  case AnimationEnum::play_start:
    return enum_name(start, value);
  case AnimationEnum::play_end:
    return enum_name(end, value);
  case AnimationEnum::stop_animation:
    return enum_name(stop, value);
  case AnimationEnum::variable_modifier:
    return enum_name(modifier, value);
  case AnimationEnum::variable_comparison:
    return enum_name(comparison, value);
  case AnimationEnum::timer:
    return enum_name(timer, value);
  case AnimationEnum::script_trigger:
  case AnimationEnum::animation_trigger:
    return enum_name(transition, value);
  case AnimationEnum::key_trigger:
    return enum_name(key, value);
  case AnimationEnum::hotspot_trigger:
    return enum_name(hotspot, value);
  case AnimationEnum::hotspot_type:
    return enum_name(shape, value);
  }
  return {};
}
std::vector<std::string> split_saved_item_path(std::string_view text,
                                               uint64_t max_parts) {
  detail::require(text.find('\0') == text.npos, "saved item path contains NUL");
  std::vector<std::string> result;
  if (text.empty())
    return result;
  size_t start = 0;
  for (;;) {
    detail::require(max_parts > 0, "saved item path part limit");
    --max_parts;
    const auto end = text.find('\n', start);
    result.emplace_back(
        text.substr(start, end == text.npos ? end : end - start));
    if (end == text.npos)
      break;
    start = end + 1;
  }
  return result;
}
struct SavedItemPathIndex::Impl {
  struct Target {
    Id first = none;
    bool duplicate = false;
  };
  struct Tree {
    Id block = none;
    bool available = false, ambiguous = false;
    // string_view keys in resolve borrow its input; owned keys live here.
    std::map<Id, std::map<std::string, Target, std::less<>>> named;
    std::map<Id, Target> null_names;
  };
  std::map<std::string, std::map<std::string, Tree, std::less<>>, std::less<>>
      trees;
};
SavedItemPathIndex::SavedItemPathIndex(const ProductData &products,
                                       uint64_t max_items) {
  detail::require(products.chunks.size() == products.blocks.size(),
                  "saved path index chunk table mismatch");
  auto index = std::make_shared<Impl>();
  for (size_t block = 0; block < products.blocks.size(); ++block) {
    const auto &b = products.blocks[block];
    const auto *saved = std::get_if<SavedItems>(&b.value);
    detail::require(block < none, "saved path index block limit");
    auto chunk = std::string_view(products.chunks[block].name);
    const auto split = chunk.rfind('\\');
    const auto element = split == chunk.npos ? chunk : chunk.substr(split + 1);
    auto &scope = index->trees[std::string(namespace_of(chunk))];
    auto [it, inserted] = scope.try_emplace(std::string(element));
    auto &tree = it->second;
    if (!inserted) {
      tree.ambiguous = true;
      continue;
    }
    tree.block = static_cast<Id>(block);
    if (b.status != ProductStatus::decoded || !saved)
      continue;
    detail::require(saved->items.size() <= max_items &&
                        saved->items.size() < none,
                    "saved path index item limit");
    max_items -= saved->items.size();
    std::vector<bool> visible(saved->items.size());
    for (Id id = 0; id < saved->items.size(); ++id) {
      const auto &item = saved->items[id];
      detail::require(item.parent == none || item.parent < id,
                      "saved path index parent order");
      detail::require(item.name.find('\0') == item.name.npos &&
                          (!item.name_is_null || item.name.empty()),
                      "saved path index invalid name");
      if (item.child_list != 0 ||
          (item.parent != none && !visible[item.parent]))
        continue;
      visible[id] = true;
      auto &target = item.name_is_null ? tree.null_names[item.parent]
                                       : tree.named[item.parent][item.name];
      if (target.first == none)
        target.first = id;
      else
        target.duplicate = true;
    }
    tree.available = true;
  }
  impl_ = std::move(index);
}
SavedItemPathResult SavedItemPathIndex::resolve(
    std::string_view owner_chunk, std::string_view element_name,
    std::string_view path, SavedItemPathSemantics semantics) const {
  detail::require(semantics == SavedItemPathSemantics::require_context ||
                      semantics == SavedItemPathSemantics::modern ||
                      semantics == SavedItemPathSemantics::legacy_stop_at_empty,
                  "invalid saved item path semantics");
  detail::require(element_name.find('\0') == element_name.npos &&
                      path.find('\0') == path.npos,
                  "saved item reference contains NUL");
  SavedItemPathResult out;
  if (element_name.empty())
    return out;
  auto scope = impl_->trees.find(namespace_of(owner_chunk));
  if (scope == impl_->trees.end())
    return out;
  auto found = scope->second.find(element_name);
  if (found == scope->second.end())
    return out;
  const auto &tree = found->second;
  if (tree.ambiguous) {
    out.status = SavedItemPathStatus::ambiguous_element;
    return out;
  }
  out.block = tree.block;
  if (!tree.available) {
    out.status = SavedItemPathStatus::unavailable;
    return out;
  }
  if (path.empty()) {
    out.status = SavedItemPathStatus::element_root;
    return out;
  }
  Id parent = none;
  size_t start = 0;
  for (;;) {
    auto end = path.find('\n', start);
    auto component = path.substr(start, end == path.npos ? end : end - start);
    const Impl::Target *target = nullptr;
    if (component.empty()) {
      if (semantics == SavedItemPathSemantics::require_context) {
        out.status = SavedItemPathStatus::context_required;
        return out;
      }
      if (semantics == SavedItemPathSemantics::legacy_stop_at_empty) {
        out.item = parent;
        out.status = parent == none ? SavedItemPathStatus::element_root
                                    : SavedItemPathStatus::resolved;
        return out;
      }
      const auto null = tree.null_names.find(parent);
      if (null != tree.null_names.end())
        target = &null->second;
    }
    if (!target) {
      auto children = tree.named.find(parent);
      if (children != tree.named.end()) {
        auto child = children->second.find(component);
        if (child != children->second.end())
          target = &child->second;
      }
    }
    if (!target)
      return out;
    out.duplicate_names |= target->duplicate;
    parent = target->first;
    if (end == path.npos)
      break;
    start = end + 1;
  }
  out.status = SavedItemPathStatus::resolved;
  out.item = parent;
  return out;
}
} // namespace nwd
