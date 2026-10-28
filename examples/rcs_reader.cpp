#include <nwd/reader.hpp>
#include <cstdint>
#include <iostream>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: rcs_reader <scan.rcs>\n";
    return 2;
  }
  try {
    nwd::RcsReader reader(argv[1]);
    const auto &index = reader.index();
    uint64_t point_count = 0;
    for (uint32_t i = 0; i < index.voxels.size(); ++i) {
      const auto &voxel = index.voxels[i];
      point_count += voxel.point_count;
      if (voxel.point_count == 0)
        continue;
      const auto point = reader.point(i, 0);
      const auto normal = nwd::decode_rcs_normal(point.normal);
      std::cout << "voxel " << i << ": " << voxel.point_count
                << " points; first XYZ = " << point.position[0] << ", "
                << point.position[1] << ", " << point.position[2]
                << "; first normal = " << normal[0] << ", " << normal[1]
                << ", " << normal[2] << '\n';
    }
    std::cout << "RCS " << index.version_major << '.' << index.version_minor
              << ": " << index.voxels.size() << " voxels, " << point_count
              << " points\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
