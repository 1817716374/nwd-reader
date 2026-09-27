#include <nwd/reader.hpp>
#include <iostream>

static int inspect(const std::filesystem::path &path) {
  try {
    const auto scene = nwd::Document(path).read_scene();
    for (const auto &model : scene.models) {
      std::cout << model.name << ": " << model.geometries.size()
                << " geometries, " << model.instances.size() << " instances\n";
      if (!model.inline_projection)
        continue;
      const auto &index = *model.inline_partition;
      const auto &graph = model.graphs.at(0);
      for (const auto &binding : model.inline_projection->instances) {
        std::cout << "shape owner " << binding.owner;
        if (binding.value == nwd::none) {
          std::cout << ": unbound (see inline_partition->shape_bindings)\n";
          continue;
        }
        const auto &instance = model.instances.at(binding.value);
        const auto source = nwd::path_reference(model, instance.path);
        std::cout << ": instance " << binding.value << ", path " << instance.path
                  << ", node graph " << source.graph << ", object "
                  << source.object << '\n';
      }
      std::cout << "spatial occurrences: " << index.spatial_occurrences.size()
                << ", source objects: " << graph.objects.size() << '\n';
      for (auto owner : model.inline_projection->legacy_appearance_fields)
        std::cout << "appearance " << owner << ": extra fields retained in arena\n";
    }
    for (const auto &warning : scene.warnings)
      std::cerr << warning << '\n';
    for (size_t i = 0; i < scene.chunks.size(); ++i)
      if (!scene.parsed_chunks[i])
        std::cout << "unconsumed chunk: " << scene.chunks[i].name << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  if (argc != 2) {
    std::cerr << "Usage: nwd_inline_model_example model.nwd\n";
    return 2;
  }
  return inspect(argv[1]);
}
