#include <nwd/reader.hpp>
#include <iostream>
#include <string_view>

static int run(const std::filesystem::path &path,
               nwd::ClipLoadProfile profile) {
  nwd::Document document(path);
  const auto products = document.read_products();
  size_t sets = 0, equations = 0;
  bool incomplete = false;
  const auto print = [&](const std::optional<nwd::LoadedClipSet> &loaded) {
    if (!loaded)
      return;
    ++sets;
    const auto active = nwd::active_clip_planes(*loaded);
    equations += active.count;
    std::cout << "  version=" << loaded->wire_version
              << " declared=" << loaded->serialized_plane_count
              << " mode=" << loaded->mode << " enabled=" << loaded->enabled
              << " active=" << active.count
              << " box_reset=" << loaded->box_reset
              << " box_defaulted=" << loaded->box_defaulted
              << " orientation_defaulted=" << loaded->orientation_defaulted
              << " orientation_nan_reset=" << loaded->orientation_nan_reset
              << '\n';
    for (uint32_t i = 0; i < active.count; ++i) {
      const auto &p = active.planes[i];
      std::cout << "    slot=" << p.slot << " normal=" << p.normal[0] << ','
                << p.normal[1] << ',' << p.normal[2]
                << " distance=" << p.distance << '\n';
    }
  };
  for (size_t b = 0; b < products.blocks.size(); ++b) {
    const auto &block = products.blocks[b];
    if (block.status == nwd::ProductStatus::partial ||
        block.status == nwd::ProductStatus::failed) {
      incomplete = true;
      std::cerr << products.chunks[b].name << ": " << block.diagnostic << '\n';
    }
    try {
      if (const auto *view = std::get_if<nwd::CurrentView>(&block.value)) {
        std::cout << products.chunks[b].name << '\n';
        print(view->loaded_clipping(profile));
      }
      if (const auto *saved = std::get_if<nwd::SavedItems>(&block.value)) {
        for (size_t i = 0; i < saved->items.size(); ++i) {
          const auto &item = saved->items[i];
          if (!item.view && !item.keyframe)
            continue;
          std::cout << products.chunks[b].name << " item=" << i << '\n';
          if (item.view)
            print(item.view->loaded_clipping(profile));
          if (item.keyframe)
            print(item.keyframe->loaded_clipping(document.version(), profile));
        }
      }
    } catch (const nwd::Error &e) {
      incomplete = true;
      std::cerr << products.chunks[b].name << ": " << e.what() << '\n';
    }
  }
  std::cout << "clip sets=" << sets << " active equations=" << equations
            << '\n';
  return incomplete ? 3 : 0;
}

template <class Char> static int entry(int argc, Char **argv) {
  try {
    if (argc != 3)
      throw nwd::Error(
          "Usage: nwd_clipping_example <file.nwd|file.nwc> <2017|2026>");
    const auto profile = std::filesystem::path(argv[2]).string();
    if (profile != "2017" && profile != "2026")
      throw nwd::Error(
          "Choose an explicit clipping load profile: 2017 or 2026");
    return run(std::filesystem::path(argv[1]),
               profile == "2017" ? nwd::ClipLoadProfile::reader_2017
                                 : nwd::ClipLoadProfile::reader_2026);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) { return entry(argc, argv); }
#else
int main(int argc, char **argv) { return entry(argc, argv); }
#endif
