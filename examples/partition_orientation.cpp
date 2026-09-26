#include <nwd/reader.hpp>
#include <iomanip>
#include <iostream>

static int run(const std::filesystem::path &path) {
  nwd::Document document(path);
  const auto scene = document.read_scene();
  std::cout << std::setprecision(17) << "wire_version=" << scene.version
            << '\n';
  for (size_t i = 0; i < scene.models.size(); ++i) {
    const auto &model = scene.models[i];
    const auto &partition = nwd::path_object(model, 0);
    const auto fields =
        nwd::partition_orientation_fields(partition, scene.version);
    auto vector = [](const std::array<double, 3> &v) {
      std::cout << v[0] << ',' << v[1] << ',' << v[2];
    };
    std::cout << "model=" << i << " up=";
    vector(fields.up);
    std::cout << " north=";
    vector(fields.north);
    std::cout << " front=";
    if (fields.front)
      vector(*fields.front);
    else
      std::cout << "absent";
    std::cout << '\n';
  }
  return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  try {
    if (argc != 2) {
      std::cerr
          << "Usage: nwd_partition_orientation_example model.nwd|model.nwc\n";
      return 2;
    }
    return run(argv[1]);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
