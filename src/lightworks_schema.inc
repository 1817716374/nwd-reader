// Wire schemas and field names independently recovered from the reader ABI.
void add_known_definitions(LightWorksArchive &out) {
  const LightWorksDefinition known[] = {
      {18,
       117768192,
       "LtImage",
       false,
       {{0, 47, ""},
        {1, 2, "width"},
        {2, 2, "height"},
        {3, 2, "bpp"},
        {4, 2, "xoffset"},
        {5, 2, "yoffset"},
        {6, 2, "row_offset"},
        {7, 2, "xmin"},
        {8, 2, "ymin"},
        {9, 2, "xmax"},
        {10, 2, "ymax"},
        {11, 5, "codec"},
        {12, 25, "samples"},
        {13, 5, "sampling_type"},
        {14, 5, "format"}}},
      {64,
       117768192,
       "Shader",
       false,
       {{1, 2, "version"},
        {2, 5, "class"},
        {3, 10, "type_name"},
        {4, 10, "plugin_url"},
        {5, 6, "sealed"},
        {6, 39, "misc_flags"}}},
      {67,
       117834496,
       "Arg",
       false,
       {{1, 10, "name"},
        {2, 5, "type"},
        {3, 6, "sealed"},
        {5, 39, "flags"},
        {4, 0, "value"}}},
      {1507338, 0, "Entity", false, {{1, 10, "name"}}},
      {1507340,
       0,
       "EntityItem",
       false,
       {{1, 10, "class_name"},
        {2, 39, "flags"},
        {3, 2, "id"},
        {4, 2, "user_spec"},
        {5, 2, "version"},
        {6, 10, "licence_id"}}},
      {1507345,
       117768192,
       "MaterialEntity",
       false,
       {{1, 3, "scale"}, {2, 39, "mat_flags"}, {3, 6, "user_image"}}},
      {1507346, 0, "ShaderEntity", false, {}},
      {1507347, 0, "TextureSpaceEntity", false, {{1, 39, "tex_space_flags"}}},
      {1507349, 117768192, "BackgroundEntity", false, {{1, 6, "user_image"}}},
      {1507350,
       0,
       "LightEntity",
       false,
       {{1, 39, "light_flags"}, {2, 39, "light_usage"}}},
      {1507429, 117833728, "ShaderDesc", false, {{1, 5, "shader_version"}}},
      {16777212,
       0,
       "LWARoot",
       false,
       {{1, 10, "xmlns:xsi"}, {2, 10, "xsi:noNamespaceSchemaLocation"}}},
  };
  for (const auto &d : known) {
    auto it = std::find_if(out.definitions.begin(), out.definitions.end(),
                           [&](const auto &x) { return x.type == d.type; });
    if (it == out.definitions.end()) {
      out.definitions.push_back(d);
      continue;
    }
    if (it->name.empty())
      it->name = d.name;
    for (auto &f : it->fields) {
      auto k =
          std::find_if(d.fields.begin(), d.fields.end(), [&](const auto &x) {
            return x.id == f.id && x.type == f.type;
          });
      if (k != d.fields.end() && f.name.empty())
        f.name = k->name;
    }
  }
}
