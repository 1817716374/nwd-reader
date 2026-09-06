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
      if (const auto *saved = std::get_if<nwd::SavedItems>(&block.value)) {
        for (const auto &item : saved->items) {
          std::cout << "  type=" << item.type << " parent=" << item.parent
                    << " name=" << item.name << " complete=" << item.complete
                    << '\n';
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
