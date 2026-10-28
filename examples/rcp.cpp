#include <nwd/reader.hpp>
#include <iostream>

namespace {
std::string_view status_name(nwd::RcpExternalRcsStatus status) {
  switch (status) {
  case nwd::RcpExternalRcsStatus::disabled: return "disabled";
  case nwd::RcpExternalRcsStatus::missing_path: return "missing_path";
  case nwd::RcpExternalRcsStatus::missing: return "missing";
  case nwd::RcpExternalRcsStatus::ambiguous: return "ambiguous";
  case nwd::RcpExternalRcsStatus::loaded: return "loaded";
  case nwd::RcpExternalRcsStatus::invalid: return "invalid";
  }
  return "unknown";
}
std::string_view status_name(nwd::RcpIdentityLinkStatus status) {
  switch (status) {
  case nwd::RcpIdentityLinkStatus::missing_id: return "missing_id";
  case nwd::RcpIdentityLinkStatus::no_match: return "no_match";
  case nwd::RcpIdentityLinkStatus::unique: return "unique";
  case nwd::RcpIdentityLinkStatus::ambiguous: return "ambiguous";
  }
  return "unknown";
}
void print_vector_fields(std::string_view label,
                         const nwd::RcpProjectVector3 &fields) {
  std::cout << "      " << label << " xyz=(";
  for (size_t i = 0; i < fields.lowercase_xyz.size(); ++i) {
    if (i)
      std::cout << ',';
    std::cout << fields.lowercase_xyz[i].value_or(std::string_view{});
  }
  std::cout << ") XYZ=(";
  for (size_t i = 0; i < fields.uppercase_xyz.size(); ++i) {
    if (i)
      std::cout << ',';
    std::cout << fields.uppercase_xyz[i].value_or(std::string_view{});
  }
  std::cout << ")\n";
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2) {
      std::cerr << "usage: nwd_rcp_example project.rcp [search-directory ...]\n";
      return 2;
    }
    nwd::RcpProjectOptions options;
    for (int i = 2; i < argc; ++i)
      options.search_paths.emplace_back(argv[i]);
    nwd::RcpProjectReader reader(argv[1], std::move(options));
    const auto &archive = reader.archive();
    std::cout << "members: " << archive.entries().size() << '\n';
    for (uint32_t i = 0; i < archive.entries().size(); ++i) {
      const auto &entry = archive.entries()[i];
      std::cout << i << "\t" << entry.name << "\tmethod="
                << entry.compression_method << "\tcompressed="
                << entry.compressed_size << "\tdecoded="
                << entry.uncompressed_size << '\n';
      if (entry.directory || entry.name.size() < 4 ||
          entry.name.substr(entry.name.size() - 4) != ".xml") {
        archive.read_entry(i); // verify non-project payload CRCs
        continue;
      }
      const auto &project = reader.project();
      const auto &xml = project.xml();
      const auto nodes = xml.nodes();
      std::cout << "  root=" << nodes[xml.root()].name;
      if (const auto uuid = project.project_uuid())
        std::cout << " project_uuid=" << *uuid;
      std::cout << '\n';
      std::cout << "  VoxelTreeRunTime records="
                << project.voxel_tree_runtime().size() << '\n';
      const auto runtime = project.voxel_tree_runtime();
      const auto external_files = reader.external_rcs_files();
      for (uint32_t runtime_index = 0; runtime_index < runtime.size();
           ++runtime_index) {
        const auto &record = runtime[runtime_index];
        std::cout << "    id=" << record.id.value_or(std::string_view{})
                  << " name=" << record.node_name.value_or(std::string_view{})
                  << " path="
                  << record.relative_path.value_or(std::string_view{})
                  << " scan=";
        if (record.scan_index)
          std::cout << *record.scan_index;
        else
          std::cout << "unresolved";
        const auto &external = external_files[runtime_index];
        std::cout << " RCS=" << status_name(external.status);
        if (external.rcs)
          std::cout << " voxels=" << external.rcs->index().voxels.size();
        else if (!external.diagnostic.empty())
          std::cout << " (" << external.diagnostic << ')';
        std::cout << '\n';
      }
      std::cout << "  Scan records=" << project.scans().size() << '\n';
      std::cout << "  ShotInfo records=" << project.shots().size()
                << " GroupInfo records=" << project.groups().size() << '\n';
      for (const auto &shot : project.shots()) {
        std::cout << "    shot=" << shot.name.value_or(std::string_view{})
                  << " point_cloud_id="
                  << shot.point_cloud_id.value_or(std::string_view{})
                  << " runtime=" << status_name(shot.runtime_link);
        if (shot.voxel_tree_runtime_index)
          std::cout << ':' << *shot.voxel_tree_runtime_index;
        std::cout << " group=" << status_name(shot.group_link);
        if (shot.group_index)
          std::cout << ':' << *shot.group_index;
        std::cout << '\n';
      }
      std::cout << "  Navigator saved views="
                << project.navigator_view_states().size()
                << " full views=" << project.saved_view_states().size()
                << '\n';
      for (const auto &state : project.navigator_view_states()) {
        std::cout << "    view=" << state.title.value_or(std::string_view{})
                  << " link=" << status_name(state.saved_view_link);
        if (state.saved_view_index)
          std::cout << ':' << *state.saved_view_index;
        std::cout << '\n';
      }
      for (const auto &view : project.saved_view_states()) {
        std::cout << "    saved-view="
                  << view.title.value_or(std::string_view{}) << " fov="
                  << view.fov.value_or(std::string_view{}) << " scans="
                  << view.scan_locations.size() << '\n';
        print_vector_fields("Eye", view.camera_eye);
        print_vector_fields("Center", view.camera_center);
        print_vector_fields("Up", view.camera_up);
        for (const auto &scan : view.scan_locations) {
          std::cout << "      scan=" << scan.id.value_or(std::string_view{})
                    << " visible="
                    << scan.visible.value_or(std::string_view{}) << " link="
                    << status_name(scan.scan_link);
          if (scan.scan_index)
            std::cout << ':' << *scan.scan_index;
          std::cout << '\n';
        }
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
