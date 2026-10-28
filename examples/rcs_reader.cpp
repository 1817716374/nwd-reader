#include <nwd/reader.hpp>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <variant>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: rcs_reader <scan.rcs>\n";
    return 2;
  }
  try {
    nwd::RcsReader reader(argv[1]);
    const auto &index = reader.index();
    const auto umbrella_levels = reader.raw_umbrella_octree_levels();
    uint64_t umbrella_entries = 0;
    uint64_t raw_chunk_table_bytes = 0;
    for (size_t level = 0; level < umbrella_levels.size(); ++level)
      umbrella_entries += index.umbrella_octree_counts[level];
    for (uint32_t i = 0; i < index.chunks.size(); ++i)
      raw_chunk_table_bytes += reader.raw_chunk(i).size();
    uint64_t point_count = 0;
    uint64_t timestamp_count = 0;
    uint64_t other_data_payload_bytes = 0;
    for (uint32_t i = 0; i < index.other_data_fields.size(); ++i)
      other_data_payload_bytes += reader.raw_other_data_payload(i).size();
    for (uint32_t i = 0; i < index.voxels.size(); ++i) {
      const auto &voxel = index.voxels[i];
      point_count += voxel.point_count;
      if (index.has_timestamp_chunk) {
        const auto timestamps = reader.raw_timestamps(i);
        timestamp_count += timestamps.size() / sizeof(double);
      }
      const auto auxiliary_tables = reader.raw_auxiliary_tables(i);
      uint64_t auxiliary_items = 0;
      for (size_t table = 0; table < auxiliary_tables.size(); ++table)
        auxiliary_items += voxel.auxiliary_counts[table];
      if (voxel.point_count == 0)
        continue;
      const auto point = reader.point(i, 0);
      const auto normal = nwd::decode_rcs_normal(point.normal);
      const auto rgba = point.color_rgba();
      std::cout << "voxel " << i << ": " << voxel.point_count
                << " points, " << auxiliary_items
                << " opaque auxiliary records; first XYZ = " << point.position[0] << ", "
                << point.position[1] << ", " << point.position[2]
                << "; RGBA = " << unsigned(rgba[0]) << ", "
                << unsigned(rgba[1]) << ", " << unsigned(rgba[2]) << ", "
                << unsigned(rgba[3])
                << "; first normal = " << normal[0] << ", " << normal[1]
                << ", " << normal[2]
                << "; raw intensity = " << unsigned(point.raw_intensity())
                << '\n';
    }
    std::cout << "RCS " << index.version_major << '.' << index.version_minor
              << ": " << index.voxels.size() << " voxels, " << point_count
              << " points, " << umbrella_entries
              << " opaque umbrella octree records, "
              << raw_chunk_table_bytes << " raw chunk-table bytes, "
              << index.other_data_fields.size() << " chunk-11 fields ("
              << other_data_payload_bytes << " raw payload bytes), "
              << timestamp_count
              << " point timestamps\n";
    for (uint32_t i = 0; i < index.other_data_fields.size(); ++i) {
      const auto &field = index.other_data_fields[i];
      if (field.native_value_kind == nwd::RcsOtherDataValueKind::opaque ||
          !field.payload_size_matches_native)
        continue;
      const auto value = reader.other_data_value(i);
      std::cout << "chunk 11 code " << field.code << " value ";
      if (const auto *boolean = std::get_if<bool>(&value))
        std::cout << (*boolean ? "true" : "false");
      else if (const auto *float_value = std::get_if<float>(&value))
        std::cout << *float_value;
      else if (const auto *int_value = std::get_if<int32_t>(&value))
        std::cout << *int_value;
      else if (const auto *double_value = std::get_if<double>(&value))
        std::cout << *double_value;
      else if (const auto *bitmap =
                   std::get_if<std::span<const uint8_t>>(&value))
        std::cout << std::count_if(bitmap->begin(), bitmap->end(),
                                   [](uint8_t byte) { return byte != 0; })
                  << " of 256 bitmap entries set";
      std::cout << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
