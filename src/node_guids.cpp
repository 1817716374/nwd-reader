#include "internal.hpp"
#include <map>
namespace nwd {
std::optional<uint32_t> node_guid_index(const Object &object) {
  size_t slot = 0;
  switch (object.type) {
  case 32:
    slot = 1;
    break;
  case 22:
  case 25:
  case 53:
    break;
  default:
    return std::nullopt;
  }
  detail::require(object.integers.size() > slot, "incomplete node GUID index");
  return object.integers[slot];
}
std::optional<std::array<uint8_t, 16>> guid_attribute(const Object &object) {
  if (object.type != 180)
    return std::nullopt;
  detail::require(object.bytes.size() == 16, "incomplete GUID attribute");
  std::array<uint8_t, 16> guid;
  std::memcpy(guid.data(), object.bytes.data(), guid.size());
  return guid;
}
struct NodeGuidIndex::Impl {
  struct Binding {
    NodeGuidStatus status = NodeGuidStatus::missing_store;
    Id block = none;
    const GuidStore *store = nullptr;
  };
  const Scene *scene = nullptr;
  std::vector<Binding> bindings;
};
NodeGuidIndex::NodeGuidIndex(const Scene &scene) {
  using detail::require;
  auto out = std::make_shared<Impl>();
  out->scene = &scene;
  require(scene.models.size() < none, "GUID model limit");
  out->bindings.resize(scene.models.size());
  std::map<std::string_view, Id, std::less<>> models;
  for (Id m = 0; m < scene.models.size(); ++m) {
    const auto &model = scene.models[m];
    auto &binding = out->bindings[m];
    auto [it, inserted] = models.try_emplace(model.name, m);
    if (!inserted) {
      binding.status = NodeGuidStatus::ambiguous_store;
      out->bindings[it->second].status = NodeGuidStatus::ambiguous_store;
    }
  }
  if (scene.products) {
    const auto &products = *scene.products;
    require(products.chunks.size() == products.blocks.size(),
            "GUID product chunk table mismatch");
    require(products.blocks.size() < none, "GUID block limit");
    for (Id b = 0; b < products.blocks.size(); ++b) {
      const auto chunk = std::string_view(products.chunks[b].name);
      const auto split = chunk.rfind('\\');
      const auto kind = split == chunk.npos ? chunk : chunk.substr(split + 1);
      if (kind != "LcOpGuidStore")
        continue;
      const auto scope =
          split == chunk.npos ? std::string_view{} : chunk.substr(0, split);
      const auto it = models.find(scope);
      if (it == models.end())
        continue;
      auto &binding = out->bindings[it->second];
      if (binding.status == NodeGuidStatus::ambiguous_store)
        continue;
      if (binding.block != none) {
        binding = {NodeGuidStatus::ambiguous_store, none, nullptr};
        continue;
      }
      binding.block = b;
      const auto &block = products.blocks[b];
      binding.store = std::get_if<GuidStore>(&block.value);
      binding.status =
          block.status == ProductStatus::decoded && binding.store
              ? (binding.store->present ? NodeGuidStatus::resolved
                                        : NodeGuidStatus::missing_store)
              : NodeGuidStatus::unavailable_store;
    }
  }
  impl_ = std::move(out);
}
NodeGuidResult NodeGuidIndex::resolve(Id model, Id path) const {
  detail::require(impl_ && model < impl_->bindings.size(),
                  "GUID model outside scene");
  const auto index =
      node_guid_index(path_object(impl_->scene->models[model], path));
  detail::require(index.has_value(), "GUID path does not name a node");
  NodeGuidResult result;
  result.index = *index;
  if (!*index)
    return result;
  const auto &binding = impl_->bindings[model];
  result.block = binding.block;
  result.status = binding.status;
  if (result.status == NodeGuidStatus::resolved) {
    if (*index > binding.store->guids.size())
      result.status = NodeGuidStatus::index_out_of_range;
    else
      result.guid = &binding.store->guids[*index - 1];
  }
  return result;
}
} // namespace nwd
