#include <nwd/reader.hpp>
#include <iostream>

int main() {
  try {
    nwd::Camera source;
    source.projection = 1;
    source.position = {1, 2, 3};
    source.orientation = {0, 0, 0, 1};
    source.parameters = {1.5, 6, .1, 1000, 0, 0};
    // Column-major affine matrix in the same coordinate units as the camera.
    const std::array<double, 16> placement{-2, 0, 0, 0, 0,  3,  0,  0,
                                           0,  0, 4, 0, 10, 20, 30, 1};
    const auto transformed = nwd::transformed_camera(source, placement);
    std::cout << "position=" << transformed.position[0] << ','
              << transformed.position[1] << ',' << transformed.position[2]
              << " height=" << transformed.height_field()
              << " near=" << transformed.near_distance()
              << " far=" << transformed.far_distance() << '\n';
    return 0;
  } catch (const nwd::Error &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
