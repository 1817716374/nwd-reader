#include "internal.hpp"
#include "json.hpp"
namespace nwd {
AssetDescription describe_asset(const Asset &asset) {
  AssetDescription out;
  if (asset.json.empty())
    return out;
  try {
    auto j = nlohmann::json::parse(asset.json);
    detail::require(j.at("version") == 2, "unsupported asset JSON version");
    if (j.contains("userassets"))
      out.roots = j.at("userassets").get<std::vector<std::string>>();
    for (const auto &[id, v] : j.at("materials").items()) {
      AssetNode node;
      node.id = id;
      node.definition = v.value("definition", std::string{});
      node.name = v.value("tag", std::string{});
      if (v.contains("properties"))
        for (const auto &[cat, props] : v.at("properties").items()) {
          if (!props.is_object())
            continue;
          for (const auto &[name, p] : props.items()) {
            if (!p.is_object())
              continue;
            AssetParameter a;
            a.category = cat;
            a.name = name;
            if (p.contains("values"))
              a.values_json = p.at("values").dump();
            if (p.contains("connections"))
              a.connections =
                  p.at("connections").get<std::vector<std::string>>();
            a.connections_enabled = p.value("IsConnectionEnabled", true);
            node.parameters.push_back(std::move(a));
            if (cat == "uris" && p.contains("values"))
              for (const auto &u : p.at("values"))
                if (u.is_string())
                  out.uris.push_back(
                      {id, name, u.get<std::string>(), name == "thumbnail"});
          }
        }
      out.nodes.push_back(std::move(node));
    }
    return out;
  } catch (const nlohmann::json::exception &e) {
    throw Error(std::string("invalid asset JSON: ") + e.what());
  }
}
} // namespace nwd
