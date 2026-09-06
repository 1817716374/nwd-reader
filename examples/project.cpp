#include <nwd/reader.hpp>
#include <iostream>
static int run(const std::filesystem::path &file) {
  try {
    nwd::ProjectOptions options;
    options.reader.threads = 8;
    // options.search_paths.push_back(texture_directory);
    // options.remaps.emplace_back(original_name,local_file);
    auto project = nwd::load_project(file, options);
    for (const auto &w : project.warnings)
      std::cerr << w << '\n';
    for (nwd::Id id = 0; id < project.nodes.size(); ++id) {
      const auto &node = project.nodes[id];
      if (node.model == nwd::none)
        continue;
      const auto &model =
          project.sources[node.source].scene->models[node.model];
      nwd::ModelIndex index(model); // Reuse for all structural-tree queries.
      std::cout << model.name << ": " << model.geometries.size()
                << " shared geometries, " << model.instances.size()
                << " instances, " << model.paths.size() << " paths\n";
      if (!model.instances.empty()) {
        auto ref = nwd::effective_appearance(project, id, 0);
        if (ref.appearance != nwd::none) {
          const auto &arena = nwd::appearance_arena(project, ref);
          const auto &a = arena.appearances.at(ref.appearance);
          if (a.material != nwd::none) {
            const auto &m = arena.materials.at(a.material);
            auto rgb = m.diffuse();
            std::cout << "diffuse " << rgb[0] << ' ' << rgb[1] << ' ' << rgb[2]
                      << " transparency " << m.transparency() << '\n';
          }
          if (a.asset != nwd::none) {
            auto graph = nwd::describe_asset(arena.assets.at(a.asset));
            std::cout << graph.nodes.size() << " asset nodes, "
                      << graph.uris.size() << " URIs\n";
          }
        }
        // Throws for explicitly unvalidated NWF placements; inspect raw
        // reference.
        auto matrix = nwd::project_world_matrix(project, id, 0);
        (void)matrix;
      }
    }
    // bytes owns/aliases immutable source storage; no image conversion is
    // required.
    for (const auto &t : project.textures)
      if (t.bytes)
        std::cout << t.alias << ": " << t.bytes->size() << " bytes\n";
    return project.complete ? 0 : 3;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  if (argc != 2) {
    std::cerr << "nwd_project_example INPUT\n";
    return 2;
  }
  return run(std::filesystem::path(argv[1]));
}
