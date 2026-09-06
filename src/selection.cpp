#include "internal.hpp"
#include <map>
namespace nwd {
namespace {
std::string_view namespace_of(std::string_view chunk) {
  auto split = chunk.rfind('\\');
  return split == chunk.npos ? std::string_view{} : chunk.substr(0, split);
}
std::string locator_part(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '?' && i + 1 < text.size()) {
      const char next = text[i + 1];
      if (next >= '1' && next <= '3') {
        out.push_back(next == '1' ? '/' : next == '2' ? ';' : '?');
        ++i;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}
} // namespace
SelectionLocator parse_selection_locator(std::string_view text,
                                         uint64_t max_parts) {
  detail::require(text.find('\0') == text.npos,
                  "selection locator contains NUL");
  SelectionLocator out;
  if (text.empty())
    return out;
  if (text == "/") {
    out.select_all = true;
    return out;
  }
  size_t begin = 0;
  for (;;) {
    auto end = text.find(';', begin);
    if (end == text.npos)
      end = text.size();
    auto segment = text.substr(begin, end - begin);
    detail::require(max_parts > 0, "selection locator part limit");
    --max_parts;
    auto &path = out.paths.emplace_back();
    auto slash = segment.find('/');
    path.root = locator_part(segment.substr(0, slash));
    while (slash != segment.npos) {
      auto next = segment.find('/', slash + 1);
      detail::require(max_parts > 0, "selection locator part limit");
      --max_parts;
      path.parts.push_back(locator_part(segment.substr(
          slash + 1, next == segment.npos ? next : next - slash - 1)));
      slash = next;
    }
    if (end == text.size())
      break;
    begin = end + 1;
  }
  return out;
}
struct SelectionSetIndex::Impl {
  struct Tree {
    Id block = none;
    bool ambiguous = false, available = false;
    std::map<std::pair<Id, std::string>, Id> children;
  };
  std::map<std::string, Tree, std::less<>> trees;
};
SelectionSetIndex::SelectionSetIndex(const ProductData &products,
                                     uint64_t max_items) {
  detail::require(products.chunks.size() == products.blocks.size(),
                  "selection index chunk table mismatch");
  auto index = std::make_shared<Impl>();
  for (size_t block = 0; block < products.blocks.size(); ++block) {
    auto chunk = std::string_view(products.chunks[block].name);
    auto split = chunk.rfind('\\');
    auto kind = split == chunk.npos ? chunk : chunk.substr(split + 1);
    if (kind != "LcOpSelectionSetsElement")
      continue;
    detail::require(block < none, "selection index block ID limit");
    auto [it, inserted] =
        index->trees.try_emplace(std::string(namespace_of(chunk)));
    auto &tree = it->second;
    if (!inserted) {
      tree.ambiguous = true;
      continue;
    }
    tree.block = static_cast<Id>(block);
    const auto &b = products.blocks[block];
    const auto *saved = std::get_if<SavedItems>(&b.value);
    if (b.status != ProductStatus::decoded || !saved)
      continue;
    detail::require(saved->items.size() <= max_items &&
                        saved->items.size() < none,
                    "selection index item limit");
    max_items -= saved->items.size();
    for (Id id = 0; id < saved->items.size(); ++id) {
      const auto &item = saved->items[id];
      detail::require(item.parent == none || item.parent < id,
                      "selection index parent order");
      auto [child, unique] =
          tree.children.emplace(std::pair{item.parent, item.name}, id);
      if (!unique)
        child->second = none;
    }
    tree.available = true;
  }
  impl_ = std::move(index);
}
SelectionLocatorResult
SelectionSetIndex::resolve(std::string_view owner_chunk,
                           const SelectionLocator &locator) const {
  detail::require(!locator.select_all || locator.paths.empty(),
                  "select-all locator also has paths");
  SelectionLocatorResult out;
  out.select_all = locator.select_all;
  auto tree_it = impl_->trees.find(namespace_of(owner_chunk));
  for (const auto &path : locator.paths) {
    auto &target = out.targets.emplace_back();
    if (path.root != "lcop_selection_set_tree") {
      target.status = SelectionTargetStatus::unsupported_root;
      continue;
    }
    if (tree_it == impl_->trees.end())
      continue;
    const auto &tree = tree_it->second;
    if (tree.ambiguous) {
      target.status = SelectionTargetStatus::ambiguous;
      continue;
    }
    target.block = tree.block;
    if (!tree.available) {
      target.status = SelectionTargetStatus::unavailable;
      continue;
    }
    target.status = SelectionTargetStatus::tree_root;
    Id parent = none;
    for (const auto &part : path.parts) {
      auto it = tree.children.find({parent, part});
      if (it == tree.children.end()) {
        target.status = SelectionTargetStatus::missing;
        break;
      }
      if (it->second == none) {
        target.status = SelectionTargetStatus::ambiguous;
        break;
      }
      parent = it->second;
      target.status = SelectionTargetStatus::resolved;
    }
    if (target.status == SelectionTargetStatus::resolved)
      target.item = parent;
  }
  return out;
}
} // namespace nwd
