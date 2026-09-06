#include <nwd/reader.hpp>
#include <iostream>
static int run(const std::filesystem::path &input) {
  try {
    nwd::Options options;
    options.threads = 4;
    nwd::Document document(input, options);
    auto scene = document.read_scene();
    for (const auto &model : scene.models) {
      std::cout << model.name << ": " << model.geometries.size()
                << " shared geometries, " << model.instances.size()
                << " instances\n";
      nwd::ModelIndex index(model);
      auto root = nwd::path_reference(model, 0);
      std::cout << "root: "
                << nwd::resolve_string(model.graphs.at(root.graph),
                                       nwd::path_object(model, 0).name)
                << "; children " << index.children(0).size() << "; index bytes "
                << index.storage_bytes() << '\n';
      if (!model.instances.empty()) {
        const auto &instance = model.instances.front();
        const auto &reference =
            model.geometry_references.at(instance.geometry_reference);
        const auto &geometry = model.geometries.at(reference.geometry);
        const auto object_ref = nwd::path_reference(model, instance.path);
        const auto &object = nwd::resolve_object(model, object_ref);
        std::cout << "instance node: "
                  << nwd::resolve_string(model.graphs.at(object_ref.graph),
                                         object.name)
                  << "; direct attribute references "
                  << object.attributes.size() << '\n';
        for (const auto &attribute : object.attributes) {
          const auto &value = nwd::resolve_object(model, attribute);
          std::cout << "attribute type " << value.type << "; properties "
                    << value.properties.size() << '\n';
        }
        if (geometry.external) {
          std::cout << "external geometry: " << geometry.external->source_path
                    << "; point/mesh bytes not decoded\n";
          continue;
        }
        auto matrix = nwd::world_matrix(model, instance);
        // Triangle indices address coordinate_indices slots, preserving
        // attribute seams.
        auto triangles = nwd::triangle_indices(geometry);
        std::cout << "first instance: " << triangles.size() / 3
                  << " triangles; translation " << matrix[12] << ", "
                  << matrix[13] << ", " << matrix[14] << " (source units)\n";
      }
    }
    return 0;
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
    std::cerr << "nwd_example INPUT\n";
    return 2;
  }
  return run(std::filesystem::path(argv[1]));
}
