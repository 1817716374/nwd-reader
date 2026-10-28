#include <nwd/reader.hpp>
#include <iostream>

int main(int argc, char **argv) {
  try {
    if (argc != 2) {
      std::cerr << "usage: nwd_rcp_example project.rcp\n";
      return 2;
    }
    nwd::RcpReader archive(argv[1]);
    std::cout << "members: " << archive.entries().size() << '\n';
    for (uint32_t i = 0; i < archive.entries().size(); ++i) {
      const auto &entry = archive.entries()[i];
      std::cout << i << "\t" << entry.name << "\tmethod="
                << entry.compression_method << "\tcompressed="
                << entry.compressed_size << "\tdecoded="
                << entry.uncompressed_size << '\n';
      auto bytes = archive.read_entry(i);
      if (entry.directory || entry.name.size() < 4 ||
          entry.name.substr(entry.name.size() - 4) != ".xml")
        continue;
      nwd::RcpProjectDocument project(bytes);
      const auto &xml = project.xml();
      const auto nodes = xml.nodes();
      std::cout << "  root=" << nodes[xml.root()].name;
      if (const auto uuid = project.project_uuid())
        std::cout << " project_uuid=" << *uuid;
      std::cout << '\n';
      std::cout << "  VoxelTreeRunTime records="
                << project.voxel_tree_runtime().size() << '\n';
      for (const auto &record : project.voxel_tree_runtime()) {
        std::cout << "    id=" << record.id.value_or(std::string_view{})
                  << " name=" << record.node_name.value_or(std::string_view{})
                  << " path="
                  << record.relative_path.value_or(std::string_view{})
                  << " scan=";
        if (record.scan_index)
          std::cout << *record.scan_index;
        else
          std::cout << "unresolved";
        std::cout << '\n';
      }
      std::cout << "  Scan records=" << project.scans().size() << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
