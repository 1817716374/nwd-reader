#include <nwd/reader.hpp>
#include <charconv>
#include <iostream>

static const char *style_status(nwd::TextStyleStatus status) {
  switch (status) {
  case nwd::TextStyleStatus::not_text:
    return "not text";
  case nwd::TextStyleStatus::null_style:
    return "null style";
  case nwd::TextStyleStatus::unavailable:
    return "unavailable";
  case nwd::TextStyleStatus::wrong_type:
    return "wrong type";
  case nwd::TextStyleStatus::resolved:
    return "resolved";
  }
  return "unknown";
}

static const char *path_status(nwd::ExternalPathStatus status) {
  switch (status) {
  case nwd::ExternalPathStatus::null_path:
    return "null path";
  case nwd::ExternalPathStatus::original:
    return "original";
  case nwd::ExternalPathStatus::remapped:
    return "remapped";
  case nwd::ExternalPathStatus::ambiguous:
    return "ambiguous";
  }
  return "unknown";
}

static int inspect(const std::filesystem::path &path,
                   std::optional<size_t> xref_block) {
  try {
    const nwd::Document document(path);
    const auto scene = document.read_scene();
    // The caller selects the applicable table. A file may contain several
    // scopes. Products and scene outlive the index and every borrowed result
    // below.
    std::optional<nwd::ProductData> products;
    std::optional<nwd::ExternalReferenceIndex> paths;
    if (xref_block) {
      products = document.read_products();
      const auto &block = products->blocks.at(*xref_block);
      const auto *table =
          std::get_if<nwd::ExternalReferenceTable>(&block.value);
      if (!table)
        throw std::runtime_error(
            "Selected block is not an external reference table");
      paths.emplace(*table);
    }
    for (const auto &model : scene.models) {
      std::cout << "Model: " << model.name << '\n';
      for (size_t i = 0; i < model.geometries.size(); ++i) {
        const auto &geometry = model.geometries[i];
        if (geometry.type == 103) {
          const auto style = nwd::resolve_text_style(model, geometry);
          std::cout << "text " << i << ": " << geometry.text
                    << ", style=" << style_status(style.status) << '\n';
          if (geometry.text_fields)
            std::cout << "  saved parameters="
                      << geometry.text_fields->parameter_count
                      << ", null text=" << geometry.text_fields->text_is_null
                      << '\n';
          if (style.status == nwd::TextStyleStatus::resolved) {
            const auto &object = style.graph->objects.at(style.object);
            const auto font = object.strings.at(0);
            std::cout << "  font="
                      << (font == nwd::none ? "(null)"
                                            : style.graph->strings.at(font))
                      << ", saved size=" << object.numbers.at(0) << '\n';
          }
        } else if (geometry.type == 104 || geometry.type == 105) {
          const auto count = geometry.type == 104 ? 10u : 13u;
          std::cout << (geometry.type == 104 ? "circle " : "cylinder ") << i
                    << ", flags=" << geometry.flags << ", saved parameters:";
          for (unsigned j = 0; j < count; ++j)
            std::cout << ' ' << geometry.parameters[j];
          std::cout << '\n';
        } else if (geometry.external) {
          const auto &external = *geometry.external;
          std::cout << "external " << i << ": loader=" << external.loader
                    << ", format=" << external.format
                    << ", saved path=" << external.source_path
                    << ", body decoded=" << external.payload_decoded
                    << ", schema=" << external.schema
                    << ", retained body bytes="
                    << external.unparsed_payload.size() << '\n';
          if (paths) {
            const auto result = paths->resolve(external);
            std::cout << "  path status=" << path_status(result.status)
                      << ", path=" << result.path
                      << ", matching entries=" << result.entries.size() << '\n';
          }
        }
      }
    }
    for (const auto &warning : scene.warnings)
      std::cerr << warning << '\n';
    for (size_t i = 0; i < scene.chunks.size(); ++i)
      if (!scene.parsed_chunks[i])
        std::cout << "unconsumed chunk " << i << ": " << scene.chunks[i].name
                  << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: nwd_geometry_descriptors_example model.nwd "
                 "[xref-block-index]\n";
    return 2;
  }
  std::optional<size_t> block;
  if (argc == 3) {
    const auto value = std::filesystem::path(argv[2]).string();
    size_t index = 0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), index);
    if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()) {
      std::cerr << "xref-block-index must be a nonnegative integer\n";
      return 2;
    }
    block = index;
  }
  return inspect(argv[1], block);
}
