#include <nwd/reader.hpp>
#include <iostream>

static const char *status_name(nwd::ProductStatus status) {
  switch (status) {
  case nwd::ProductStatus::decoded:
    return "decoded";
  case nwd::ProductStatus::partial:
    return "partial";
  case nwd::ProductStatus::failed:
    return "failed";
  default:
    return "not handled by product reader";
  }
}
static int run(const std::filesystem::path &path) {
  try {
    nwd::Options options;
    options.threads = 0;
    auto data = nwd::Document(path, options).read_products();
    bool incomplete = false;
    for (std::size_t i = 0; i < data.blocks.size(); ++i) {
      const auto &block = data.blocks[i];
      std::cout << data.chunks[i].name << ": " << status_name(block.status)
                << '\n';
      incomplete |= block.status == nwd::ProductStatus::partial ||
                    block.status == nwd::ProductStatus::failed;
      if (!block.diagnostic.empty())
        std::cerr << block.diagnostic << '\n';
      if (const auto *database =
              std::get_if<nwd::DatabaseLinks>(&block.value)) {
        std::cout << "  database links=" << database->links.size() << '\n';
        for (const auto &link : database->links)
          std::cout << "    name object=" << link.name
                    << " fields=" << link.fields.size()
                    << " connection decoded="
                    << link.tagged_connection.has_value() << '\n';
      }
      if (const auto *database = std::get_if<nwd::FileDatabase>(&block.value))
        for (const auto &table : database->tables)
          std::cout << "  table=" << table.name
                    << " columns=" << table.columns.size()
                    << " rows=" << table.rows.size()
                    << " foreign keys=" << table.foreign_keys.size() << '\n';
      if (const auto *spatial =
              std::get_if<nwd::SpatialHierarchy>(&block.value))
        std::cout << "  spatial nodes=" << spatial->nodes.size()
                  << " associations verified=" << spatial->associations_verified
                  << '\n';
      if (const auto *publish =
              std::get_if<nwd::PublishInformation>(&block.value))
        std::cout << "  title=" << publish->title
                  << " author=" << publish->author << '\n';
      if (const auto *links =
              std::get_if<nwd::HyperlinkOverrides>(&block.value))
        std::cout << "  hyperlink paths=" << links->paths.size() << '\n';
      if (const auto *grids = std::get_if<nwd::Grids>(&block.value))
        for (const auto &system : grids->systems)
          std::cout << "  grid=" << system.label
                    << " lines=" << system.lines.size()
                    << " levels=" << system.levels.size() << '\n';
      if (const auto *store = std::get_if<nwd::GuidStore>(&block.value))
        std::cout << "  GUID store present=" << store->present
                  << " entries=" << store->guids.size() << '\n';
      if (const auto *stats = std::get_if<nwd::SceneStatistics>(&block.value))
        std::cout << "  statistics text bytes=" << stats->text.size() << '\n';
      if (const auto *definitions =
              std::get_if<nwd::LegacyTimeLinerDefinitions>(&block.value)) {
        std::cout << "  legacy appearances=" << definitions->appearances.size()
                  << " task types=" << definitions->task_types.size()
                  << " default status="
                  << definitions->default_status.has_value() << '\n';
        for (const auto &task : definitions->task_types)
          std::cout << "    task type=" << task.name
                    << " states=" << task.states.size() << '\n';
      }
      if (const auto *saved = std::get_if<nwd::SavedItems>(&block.value)) {
        for (const auto &item : saved->items) {
          std::cout << "  type=" << item.type << " parent=" << item.parent
                    << " name=" << item.name << " complete=" << item.complete
                    << '\n';
          if (const auto *result = std::get_if<nwd::ClashResult>(&item.clash))
            std::cout << "    distance=" << result->distance
                      << " path links=" << result->path_links[0] << ','
                      << result->path_links[1] << '\n';
          if (item.sheet_info)
            std::cout << "    sheet=" << item.sheet_info->sheet_id << '\n';
          if (item.file_info)
            std::cout << "    default sheet matches="
                      << item.file_info->default_sheet_matches.size() << '\n';
          if (item.light)
            std::cout << "    light object=" << item.light->object
                      << " position=" << item.light->position[0] << ','
                      << item.light->position[1] << ','
                      << item.light->position[2] << '\n';
          if (item.material_asset != nwd::none)
            std::cout << "    material asset=" << item.material_asset << '\n';
          if (item.keyframe)
            std::cout << "    time=" << item.keyframe->time << '\n';
          if (const auto *task =
                  std::get_if<nwd::TimeLinerTask>(&item.timeliner))
            std::cout << "    task=" << task->display_id
                      << " progress=" << task->progress_percent << '\n';
        }
      }
    }
    // The product reader leaves core model blocks to read_scene()/read_nwf().
    // Exit success here does not imply full document coverage.
    return incomplete ? 3 : 0;
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
    std::cerr << "Usage: nwd_products_example FILE\n";
    return 2;
  }
  return run(argv[1]);
}
