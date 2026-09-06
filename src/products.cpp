#include "internal.hpp"
#include "blowfish.hpp"
namespace nwd {
namespace {
using namespace detail;
using Unsupported = UnsupportedLayout;
uint32_t count(Cursor &r, uint64_t limit) {
  auto n = r.u32();
  require(n <= limit && n <= (r.data.size() - r.pos) / 4,
          "product array count outside stream/resource limit");
  return n;
}
void guid(Cursor &r, std::array<uint8_t, 16> &out) {
  r.align(4);
  auto b = r.raw(out.size());
  std::copy(b.begin(), b.end(), out.begin());
}
bool boolean(Cursor &r) {
  auto v = r.u32();
  require(v <= 1, "product boolean");
  return v != 0;
}
std::pair<std::string, std::string> string_pair(Cursor &r) {
  auto a = r.string();
  auto b = r.string();
  return {std::move(a), std::move(b)};
}
void comments(Cursor &r, std::vector<SavedComment> &out,
              const Options &options) {
  for (auto n = count(r, options.max_objects); n; --n) {
    SavedComment v;
    v.author = r.string();
    v.text = r.string();
    v.timestamp = r.read<int64_t>();
    v.id = r.u64();
    v.status = r.read<int32_t>();
    out.push_back(std::move(v));
  }
}
void find_selection(Cursor &r, SavedSelection &v, const Options &options) {
  v.locator = r.string();
  if (v.locator.empty())
    for (auto n = count(r, options.max_objects); n; --n)
      v.path_links.push_back(r.u32());
}
void implicit_selection(Cursor &r, SavedSelection &v, ObjectReader &objects,
                        const Options &options) {
  v.kind = r.u32();
  if (v.kind == 0) {
    for (auto n = count(r, options.max_objects); n; --n)
      v.path_links.push_back(r.u32());
  } else if (v.kind == 1) {
    for (auto n = count(r, options.max_objects); n; --n) {
      SearchCondition c;
      c.category = objects.object(r);
      c.property = objects.object(r);
      c.condition = r.u32();
      c.options = r.u32();
      c.value = read_data_value(
          r, [&] { return objects.string(r); },
          [&] { return Reference{0, objects.object(r)}; });
      v.conditions.push_back(std::move(c));
    }
    find_selection(r, v, options);
    v.search_mode = r.u32();
    v.prune_below_match = boolean(r);
  } else if (v.kind == 2) {
    for (auto n = count(r, options.max_objects); n; --n)
      v.item_paths.push_back(string_pair(r));
    v.item_path_mode = r.u32();
  } else
    throw Unsupported("saved selection kind " + std::to_string(v.kind));
}
uint32_t timeliner(Cursor &r, SavedItem &s, uint32_t version,
                   ObjectReader &objects, const Options &options) {
  auto status = [&](TimeLinerStatus &v) {
    v.mode = r.u32();
    v.appearance = r.string();
  };
  switch (s.type) {
  case 60: {
    auto &v = s.timeliner.emplace<TimeLinerDataSource>();
    if (version < 224)
      break;
    v.provider_id = r.string();
    v.provider_name = r.string();
    v.provider_version = r.read<double>();
    v.sync_time = r.read<int64_t>();
    v.project = r.string();
    for (auto n = count(r, options.max_objects); n; --n)
      v.available_fields.push_back(string_pair(r));
    for (int i = 0; i < 6; ++i)
      v.field_mappings.push_back(string_pair(r));
    for (auto n = count(r, options.max_objects); n; --n)
      v.user_fields.push_back(string_pair(r));
    if (version >= 226 &&
        (v.provider_id == "TimelinerDataSource_CsvImport_NET.Navisworks" ||
         v.provider_id == "TimelinerDataSource_CSV.Navisworks")) {
      auto &csv = v.csv.emplace();
      for (auto &x : csv.fields)
        x = r.string();
      csv.first_row_header = boolean(r);
      csv.custom_date_format = boolean(r);
      csv.date_format = r.string();
      if (version >= 227)
        csv.row_count = r.read<int32_t>();
    }
    if (version >= 255)
      for (int i = 0; i < 4; ++i)
        v.field_mappings.push_back(string_pair(r));
    break;
  }
  case 61: {
    auto &v = s.timeliner.emplace<TimeLinerTask>();
    v.enabled = boolean(r);
    v.synchronization_id = r.string();
    if (version >= 231)
      v.display_id = r.string();
    for (auto &x : v.actual_dates)
      x = r.read<int64_t>();
    for (auto &x : v.actual_flags)
      x = boolean(r);
    for (auto &x : v.planned_dates)
      x = r.read<int64_t>();
    for (auto &x : v.planned_flags)
      x = boolean(r);
    v.task_type = r.string();
    v.data_source = r.string();
    if (boolean(r))
      implicit_selection(r, v.implicit_selection.emplace(), objects, options);
    if (boolean(r))
      find_selection(r, v.find_selection.emplace(), options);
    if (version < 229 && boolean(r))
      v.legacy_view = read_viewpoint(r, version);
    comments(r, v.task_comments, options);
    for (auto n = count(r, options.max_objects); n; --n)
      v.user_data.push_back(r.string());
    v.animation_behavior = r.u32();
    v.animation_path = string_pair(r);
    v.script_path = string_pair(r);
    v.progress_flag = boolean(r);
    v.progress_percent = r.read<double>();
    if (version < 248) {
      auto a = r.string();
      auto b = r.read<double>();
      v.legacy_cost.emplace(std::move(a), b);
    } else
      for (size_t i = 0; i < 4; ++i) {
        v.costs[i] = r.read<double>();
        v.cost_flags[i] = boolean(r);
      }
    return count(r, options.max_objects);
  }
  case 62:
    for (auto &v : s.timeliner.emplace<TimeLinerTaskType>().states)
      status(v);
    break;
  case 63: {
    auto &v = s.timeliner.emplace<TimeLinerAppearance>();
    for (auto &x : v.color)
      x = r.read<double>();
    v.opacity = r.read<double>();
    break;
  }
  case 64:
    status(s.timeliner.emplace<TimeLinerStatus>());
    break;
  }
  return 0;
}
AnimationKeyFrame keyframe(Cursor &r, uint32_t type, uint32_t version) {
  AnimationKeyFrame v;
  v.time = r.read<double>();
  v.interpolate = boolean(r);
  if (type == 39) {
    v.camera = read_camera(r, version);
    if (version >= 102)
      v.focal_distance = r.read<double>();
  } else if (type == 41)
    read_clip_planes(r, v.clip_planes, v.clip_set, version);
  else {
    v.flags = r.u32();
    if (v.flags & ~127u)
      throw Unsupported("animation keyframe flags");
    auto read = [&](auto &field) {
      for (auto &x : field.emplace())
        x = r.read<double>();
    };
    if (v.flags & 1)
      read(v.translation);
    if (version < 115) {
      if (v.flags & 2) {
        read(v.rotation);
        read(v.tool_orientation);
        read(v.center);
      }
    } else {
      if (v.flags & 2)
        read(v.rotation);
      if (v.flags & 64)
        read(v.tool_orientation);
      if (v.flags & 32)
        read(v.center);
    }
    if (v.flags & 4) {
      read(v.scale_size);
      read(v.scale_orientation);
    }
    if (v.flags & 8)
      read(v.color);
    if (v.flags & 16)
      v.opacity = r.read<double>();
  }
  return v;
}
Redline redline(Cursor &r, uint32_t version, const Options &options) {
  Redline v;
  auto type = r.u32();
  if (type > 5)
    throw Unsupported("redline type " + std::to_string(type));
  v.type = static_cast<RedlineType>(type);
  auto boolean = [&] {
    auto x = r.u32();
    require(x <= 1, "redline boolean");
    return x != 0;
  };
  auto point = [&] {
    std::array<double, 2> p;
    for (auto &x : p)
      x = r.read<double>();
    v.points.push_back(p);
  };
  if (v.type == RedlineType::text) {
    v.text = r.string();
    point();
  } else if (v.type == RedlineType::cloud) {
    for (auto n = count(r, options.max_objects); n; --n)
      point();
  } else {
    if (v.type == RedlineType::tag) {
      v.tag_ids[0] = r.u64();
      v.tag_extra = r.u32();
    }
    point();
    point();
    if (v.type == RedlineType::tag) {
      v.tag_position_flag = boolean();
      for (auto &x : v.tag_position)
        x = r.read<double>();
    }
  }
  v.width = r.read<int32_t>();
  for (auto &x : v.color)
    x = r.read<double>();
  if (v.type == RedlineType::tag) {
    v.tag_bounds_flag = boolean();
    for (auto &x : v.tag_bounds)
      x = r.read<double>();
    if (version >= 86)
      v.tag_ids[1] = r.u64();
  }
  if (version >= 409) {
    r.align(2);
    auto bytes = r.raw(2);
    v.style = static_cast<uint16_t>(bytes[0] | (uint16_t(bytes[1]) << 8));
  }
  return v;
}
Assignee assignee(Cursor &r, uint32_t version) {
  Assignee v;
  v.name = r.string();
  if (version >= 450 && !v.name.empty())
    v.id = r.string();
  return v;
}
ProductRule rule(Cursor &r, ObjectReader &objects, const Options &options) {
  ProductRule v;
  v.plugin = r.string();
  v.enabled = boolean(r);
  v.name = r.string();
  for (auto n = count(r, options.max_objects); n; --n) {
    auto id = r.read<int32_t>();
    auto value = read_data_value(
        r, [&] { return objects.string(r); },
        [&] { return Reference{0, objects.object(r)}; });
    v.parameters.emplace_back(id, std::move(value));
  }
  return v;
}
void issue_view(Cursor &r, SavedItem &s, uint32_t version,
                const Options &options) {
  s.view = read_viewpoint(r, version);
  s.redline_count = count(r, options.max_objects);
  for (uint32_t n = 0; n < s.redline_count; ++n)
    s.redlines.push_back(redline(r, version, options));
}
uint32_t clash(Cursor &r, SavedItem &s, uint32_t version, ObjectReader &objects,
               const Options &options, bool legacy = false) {
  if (s.type == 50) {
    auto &v = s.clash.emplace<ClashTest>();
    v.type = r.u32();
    if (v.type == 4)
      v.custom_test = r.string();
    else {
      for (size_t i = 0; i < 2; ++i) {
        find_selection(r, v.selections[i], options);
        v.self_intersect[i] = boolean(r);
      }
      for (auto &x : v.primitive_flags)
        x = r.u32();
      v.tolerance = r.read<double>();
      v.simulation_step = r.read<double>();
      v.simulation_type = r.u32();
      if (v.simulation_type > 2)
        throw Unsupported("clash simulation type");
      if (v.simulation_type == 2)
        v.animation_path = string_pair(r);
      for (auto n = count(r, options.max_objects); n; --n)
        v.rules.push_back(rule(r, objects, options));
      if (v.type == 1) {
        v.relative_touch = boolean(r);
        v.relative_tolerance = r.read<double>();
        v.absolute_tolerance = r.read<double>();
      }
    }
    v.status = r.u32();
    if (version >= 304)
      v.run_time = r.read<int64_t>();
    if (version >= 424)
      v.merge_composites = boolean(r);
    if (version >= 450) {
      v.priority = r.read<int32_t>();
      v.assignee = assignee(r, version);
    }
    return count(r, options.max_objects);
  }
  if (s.type == 52) {
    auto &v = s.clash.emplace<ClashResultGroup>();
    issue_view(r, s, version, options);
    if (legacy)
      comments(r, s.comments, options);
    if (version >= 450) {
      v.test_name = r.string();
      v.priority = r.read<int32_t>();
    }
    return count(r, options.max_objects);
  }
  auto &v = s.clash.emplace<ClashResult>();
  v.test_name = r.string();
  if (version >= 450)
    v.priority = r.read<int32_t>();
  v.distance = r.read<double>();
  for (auto &x : v.path_links)
    x = r.u32();
  if (version >= 423)
    for (auto &x : v.fallback_path_links.emplace())
      x = r.u32();
  for (auto &p : v.points)
    for (auto &x : p)
      x = r.read<double>();
  for (auto &p : v.bounds)
    for (auto &x : p)
      x = r.read<double>();
  v.created_time = r.read<int64_t>();
  v.status = r.u32();
  auto approval_status = legacy && version < 113 ? 2u : 3u;
  if (version >= 450 || v.status == approval_status) {
    v.approved_time = r.read<int64_t>();
    v.approved_by = assignee(r, version);
    if (version >= 450) {
      v.resolved_time = r.read<int64_t>();
      v.resolved_by = assignee(r, version);
    }
  }
  issue_view(r, s, version, options);
  if (legacy)
    comments(r, s.comments, options);
  auto &e = v.simulation;
  e.type = r.u32();
  if (e.type) {
    if (e.type > 2)
      throw Unsupported("clash result simulation event");
    for (auto &x : e.times)
      x = r.read<int64_t>();
    e.time = r.read<double>();
    e.name = r.string();
    if (e.type == 1)
      for (auto &x : e.task_names)
        x = r.string();
    else {
      if (version >= 98)
        e.item_paths[0] = string_pair(r);
      e.item_paths[1] = string_pair(r);
    }
  }
  if (version >= (legacy ? 228u : 303u))
    v.assigned_to = assignee(r, version);
  if (version >= 412)
    v.run_test_type = r.u32();
  return 0;
}
void legacy_clash_items(Cursor &r, SavedItems &out, Id parent, uint32_t n,
                        uint32_t version, ObjectReader &objects,
                        const Options &options, unsigned depth = 0) {
  require(depth < 128, "legacy clash nesting limit");
  for (uint32_t i = 0; i < n; ++i) {
    require(out.items.size() < options.max_objects && out.items.size() < none,
            "legacy clash item limit");
    Id id = static_cast<Id>(out.items.size());
    out.items.emplace_back();
    auto &s = out.items.back();
    s.offset = r.pos;
    s.parent = parent;
    if (parent == none)
      s.type = 50;
    else {
      auto type = version >= 106 ? r.u32() : 0;
      if (type > 1)
        throw Unsupported("legacy clash issue type " + std::to_string(type));
      s.type = type == 0 ? 51 : 52;
    }
    s.name = r.string();
    auto children = clash(r, s, version, objects, options, true);
    if (children)
      legacy_clash_items(r, out, id, children, version, objects, options,
                         depth + 1);
    out.items[id].complete = true;
    out.items[id].end_offset = r.pos;
  }
}
void items(Cursor &r, SavedItems &out, Id parent, uint32_t n, uint32_t version,
           std::span<const SchemaDefinition> schemas, const Options &options,
           ObjectReader &objects, unsigned depth, uint32_t child_list = 0,
           Id fixed_type = none) {
  require(depth < 128, "saved item nesting limit");
  for (uint32_t k = 0; k < n; ++k) {
    require(out.items.size() < options.max_objects && out.items.size() < none,
            "saved item resource limit");
    Id id = static_cast<Id>(out.items.size());
    out.items.emplace_back();
    auto &s = out.items.back();
    s.parent = parent;
    s.child_list = child_list;
    s.offset = r.pos;
    s.type = fixed_type == none ? r.u32() : fixed_type;
    s.name = r.string();
    comments(r, s.comments, options);
    if (version >= 246)
      guid(r, s.guid);
    if (version >= 410)
      for (auto c = count(r, options.max_objects); c; --c)
        s.properties.push_back(read_schema_instance(r, schemas));
    uint32_t children = 0;
    switch (s.type) {
    case 80:
      s.material_asset = objects.object(r);
      require(s.material_asset == none ||
                  out.objects.objects.at(s.material_asset).type == 185,
              "saved material asset type");
      break;
    case 70:
      s.file_info.emplace();
      children = count(r, options.max_objects);
      break;
    case 71: {
      auto &v = s.sheet_info.emplace();
      v.sheet_id = r.string();
      v.sheet_type = r.u32();
      for (auto n = count(r, options.max_objects); n; --n)
        v.property_categories.push_back(objects.object(r));
      if (version >= 431) {
        v.initial_file = r.string();
        v.initial_sheet = r.string();
      } else if (version >= 222)
        v.legacy_file_and_sheet = r.string();
      if (version >= 251)
        v.initial_sheet_display_name = r.string();
      if (version >= 246)
        guid(r, v.source_guid.emplace());
      break;
    }
    case 83: {
      auto &v = s.light.emplace();
      v.object = objects.object(r);
      require(v.object == none || out.objects.objects.at(v.object).type == 27,
              "saved light reference type");
      for (auto &x : v.position)
        x = r.read<double>();
      for (auto &x : v.target)
        x = r.read<double>();
      break;
    }
    case 0:
      s.view = read_current_view(r, version);
      // The lists have no per-item length. Stop at an unsupported list body;
      // do not scan for the next plausible camera or child header.
      s.redline_count = count(r, options.max_objects);
      for (uint32_t c = 0; c < s.redline_count; ++c)
        s.redlines.push_back(redline(r, version, options));
      s.element_record_count = count(r, options.max_objects);
      for (uint32_t c = 0; c < s.element_record_count; ++c)
        s.element_records.push_back(objects.object(r));
      break;
    case 1:
      s.animation_flags = r.u32();
      s.animation_mode = r.u32();
      children = count(r, options.max_objects);
      break;
    case 2:
      s.cut_duration = r.read<double>();
      break;
    case 4:
    case 34:
    case 53:
      children = count(r, options.max_objects);
      break;
    case 54:
    case 55:
      if (count(r, options.max_objects))
        throw Unsupported("nonempty clash field/status root");
      break;
    case 58: {
      // Validation item: a concrete selection, without a kind discriminator.
      auto &selection = s.selection.emplace();
      selection.kind = 0;
      for (auto n = count(r, options.max_objects); n; --n)
        selection.path_links.push_back(r.u32());
      break;
    }
    case 32:
    case 33:
    case 35:
    case 36:
    case 37:
    case 38:
      children = count(r, options.max_objects);
      break;
    case 39:
    case 40:
    case 41:
      s.keyframe = keyframe(r, s.type, version);
      break;
    case 60:
    case 61:
    case 62:
    case 63:
    case 64:
      children = timeliner(r, s, version, objects, options);
      break;
    case 50:
    case 51:
    case 52:
      children = clash(r, s, version, objects, options);
      break;
    case 5: {
      auto &selection = s.selection.emplace();
      implicit_selection(r, selection, objects, options);
      selection.source = r.string();
      selection.timestamp = r.read<int64_t>();
      break;
    }
    default:
      throw Unsupported("saved item type " + std::to_string(s.type));
    }
    if (children)
      items(r, out, id, children, version, schemas, options, objects,
            depth + 1);
    // Recursing can reallocate out.items. Reacquire the parent by index.
    auto type = out.items[id].type;
    if (type == 70) {
      auto &v = *out.items[id].file_info;
      v.default_sheet_id = r.string();
      for (auto n = count(r, options.max_objects); n; --n)
        v.property_categories.push_back(objects.object(r));
      if (version >= 306) {
        guid(r, v.source_guid.emplace());
        guid(r, v.file_version_guid.emplace());
      }
      for (Id j = id + 1; j < out.items.size(); ++j) {
        const auto &sheet = out.items[j];
        require(sheet.parent != id || sheet.sheet_info.has_value(),
                "file info contains non-sheet child");
        if (sheet.parent == id && sheet.sheet_info &&
            sheet.sheet_info->sheet_id == v.default_sheet_id)
          v.default_sheet_matches.push_back(j);
      }
    }
    if (type >= 32 && type <= 38 && type != 34) {
      out.items[id].animation_flags = r.u32();
      if (type == 32) {
        out.items[id].end_time = r.read<double>();
        out.items[id].infinite = boolean(r);
      } else if (type == 37)
        find_selection(r, out.items[id].selection.emplace(), options);
      else if (type == 33 || type == 35) {
        auto second = count(r, options.max_objects);
        if (second)
          items(r, out, id, second, version, schemas, options, objects,
                depth + 1, 1);
      }
    }
    out.items[id].complete = true;
    out.items[id].end_offset = r.pos;
  }
}
bool supported(std::string_view kind) {
  return kind == "LcOwPresenterElement" || kind == "LcOwLightsElement" ||
         kind == "LcOpNwdSpatialHierarchy" ||
         kind == "LcOpTextureSpaceElement" || kind == "LcOpDBCache" ||
         kind == "LcOpNwdPublish" || kind == "LcOpShadOverridesElement" ||
         kind == "LcOpNwdSerial" || kind == "LcOpNwdGeometryCompress" ||
         kind == "LcReMaterialElement" ||
         kind == "LcOpHyperlinksOverrideElement" ||
         kind == "LcOpFileDatabaseElement" ||
         kind == "LcOpFileDatabaseElementNWD" ||
         kind == "LcOpOdyScenePropsChunk" || kind == "LcOpOdyFileInfoChunk" ||
         kind == "LcOpToolElement" || kind == "LcOpGraphicsSystemElement" ||
         kind == "LcOpLightsElement" || kind == "LcOpXRefTable" ||
         kind == "LcOpNwdStats" || kind == "LcOpGuidStore" ||
         kind == "LcOpGridElement" || kind == "LcOdpDBDatabaseLinksElement" ||
         kind == "LcTlAppearanceDefinitionsElement" ||
         kind == "LcTlTaskTypeDefinitionsElement" ||
         kind == "LcTlDefaultStatusElement" ||
         kind == "LcTlSimulateDurationElement" ||
         kind == "LcTlSimulateTimeElement" ||
         kind == "LcOpCurrentAnimationElement" || kind == "LcOpClashElement" ||
         kind == "LcOpCurrentViewElement" || kind == "LcOpHomeViewElement" ||
         kind == "LcOpPlanViewElement" || kind == "LcOpSectionViewElement" ||
         kind == "LcOpBackgroundElement" || kind == "LcOpHeadlightElement" ||
         kind == "LcOpCullingElement" || kind == "LcOpSpeedElement" ||
         kind == "LcOpNextCommentIdElement" ||
         kind == "LcOpSavedViewsElement" ||
         kind == "LcOpSelectionSetsElement" ||
         kind == "LcAnSavedAnimationsElement" ||
         kind == "LcTlTimelinerElement" || kind == "LcTlGUISettingsElement" ||
         kind == "LcOpZoneElement";
}
void decode(ProductBlock &b, Cursor &r, std::string_view kind, uint32_t version,
            std::span<const SchemaDefinition> schemas, const Options &options,
            bool implicit_node_map) {
  if (version < 82)
    throw Unsupported("product version below 82");
  if (kind == "LcOpClashElement" &&
      ((version < 301 && version != 103 && version != 112) || version > 450))
    throw Unsupported("clash container version not yet validated");
  if (kind == "LcOwPresenterElement") {
    read_presenter(b.value.emplace<PresenterData>(), r, version,
                   implicit_node_map, options);
    return;
  } else if (kind == "LcOwLightsElement") {
    read_presenter_lights(b.value.emplace<PresenterLights>(), r, version,
                          options);
    return;
  } else if (kind == "LcOpClashElement" && version < 301) {
    auto &v = b.value.emplace<SavedItems>();
    ObjectReader objects(r.data, v.objects, version, options);
    for (auto n = count(r, options.max_objects); n; --n) {
      auto name = r.string();
      auto value = r.read<int32_t>();
      v.ignore_plugins.emplace_back(std::move(name), value);
    }
    read_source_references(r, v.source_references, objects, version, options);
    v.legacy_clash = true;
    v.root_count = count(r, options.max_objects);
    legacy_clash_items(r, v, none, v.root_count, version, objects, options);
    return;
  }
  if (kind == "LcOpNwdSpatialHierarchy") {
    auto &v = b.value.emplace<SpatialHierarchy>();
    struct Frame {
      Id parent;
      uint32_t remaining;
      Id last_child;
    };
    std::vector<Frame> stack{{none, 1, none}};
    v.nodes.reserve(static_cast<size_t>(
        std::min<uint64_t>(options.max_objects, r.data.size() / 8 + 1)));
    while (!stack.empty()) {
      auto &f = stack.back();
      if (!f.remaining) {
        stack.pop_back();
        continue;
      }
      require(stack.size() < 128 && v.nodes.size() < options.max_objects &&
                  v.nodes.size() < none,
              "spatial tree depth/node limit");
      Id id = static_cast<Id>(v.nodes.size());
      auto &n = v.nodes.emplace_back();
      n.parent = f.parent;
      if (f.last_child != none)
        v.nodes[f.last_child].next_sibling = id;
      else if (f.parent != none)
        v.nodes[f.parent].first_child = id;
      f.last_child = id;
      --f.remaining;
      n.type = r.u32();
      if (n.type == 1 || n.type == 4)
        n.fragment = r.u32();
      else if (n.type == 2 || n.type == 3 || n.type == 5) {
        auto children = count(r, options.max_objects);
        require(n.type != 5 || children <= 7,
                "dynamic spatial group child limit");
        if (children)
          stack.push_back({id, children, none});
      } else if (n.type != 0)
        throw Unsupported("spatial node type " + std::to_string(n.type));
    }
  } else if (kind == "LcOpTextureSpaceElement") {
    auto &v = b.value.emplace<TextureSpaceOverrides>();
    v.implicit_node_map = implicit_node_map;
    read_texture_spaces(r, v.records, version, implicit_node_map, options);
  } else if (kind == "LcOpDBCache") {
    auto &v = b.value.emplace<CacheMetadata>();
    v.version = r.read<int32_t>();
    if (v.version != 7)
      throw Unsupported("cache metadata module version");
    v.flags = r.u32();
    v.source_filename = r.string();
    v.source_sheet = r.string();
    v.source_timestamp = r.read<int64_t>();
    v.source_size = r.u64();
    ObjectReader objects(r.data, v.objects, version, options);
    uint64_t budget = std::min<uint64_t>(1000000, options.max_objects);
    read_cache_data(r, v.plugins, v.references, v.options, objects, options,
                    budget);
    v.saved_state = boolean(r);
  } else if (kind == "LcOpNwdPublish") {
    auto &v = b.value.emplace<PublishInformation>();
    ObjectReader objects(r.data, v.objects, version, options);
    v.name = objects.string(r);
    v.class_name = objects.object(r);
    v.attribute_flags = r.u32();
    if (v.attribute_flags & 0x10000)
      throw Unsupported("publish attribute auxiliary properties");
    v.flags = r.u32();
    v.title = r.string();
    v.subject = r.string();
    v.author = r.string();
    v.publisher = r.string();
    v.published = r.read<int64_t>();
    v.expires = r.read<int64_t>();
    v.copyright = r.string();
    v.published_for = r.string();
    v.comments = r.string();
    v.keywords = r.string();
  } else if (kind == "LcOpShadOverridesElement") {
    auto &v = b.value.emplace<NodeOverrides>();
    if (version >= 437) {
      for (auto n = count(r, options.max_objects); n; --n) {
        auto &entry = v.paths.emplace_back();
        entry.path_link = r.u32();
        entry.flags = r.byte();
        entry.flags |= uint32_t(r.byte()) << 8;
      }
      if (count(r, options.max_objects))
        throw Unsupported(
            "node override node dictionary needs source path-map context");
    } else {
      for (;;) {
        auto flags = r.u32();
        if (!flags)
          break;
        require(v.paths.size() < options.max_objects,
                "node override resource limit");
        v.paths.push_back({r.u32(), flags});
      }
    }
  } else if (kind == "LcOpNwdSerial") {
    b.value.emplace<FileSerial>().value = r.string();
  } else if (kind == "LcOpNwdGeometryCompress") {
    auto &v = b.value.emplace<GeometryCompression>();
    v.flags = r.u32();
    v.normal_precision = r.byte();
    v.color_precision = r.byte();
    v.texture_coordinate_precision = r.byte();
    v.coordinate_precision = r.read<float>();
  } else if (kind == "LcOpFileDatabaseElement" ||
             kind == "LcOpFileDatabaseElementNWD") {
    read_file_database(b.value.emplace<FileDatabase>(), r,
                       kind == "LcOpFileDatabaseElement", options);
  } else if (kind == "LcOpHyperlinksOverrideElement") {
    auto &v = b.value.emplace<HyperlinkOverrides>();
    ObjectReader objects(r.data, v.objects, version, options);
    for (auto n = count(r, options.max_objects); n; --n) {
      auto &entry = v.paths.emplace_back();
      entry.path_link = r.u32();
      for (auto k = count(r, options.max_objects); k; --k) {
        auto &link = entry.links.emplace_back();
        link.url = r.string();
        link.label = r.string();
        link.category = objects.object(r);
        require(link.category == none ||
                    v.objects.objects.at(link.category).type == 52,
                "hyperlink category type");
        for (auto p = count(r, options.max_objects); p; --p) {
          auto &xyz = link.positions.emplace_back();
          for (auto &x : xyz)
            x = r.read<double>();
        }
      }
    }
    if (count(r, options.max_objects))
      throw Unsupported(
          "hyperlink node dictionary needs its source path-map context");
  } else if (kind == "LcOpOdyScenePropsChunk") {
    auto &v = b.value.emplace<SceneProperties>();
    v.saved_filename = r.string();
    if (version >= 402)
      guid(r, v.guid.emplace());
    v.legacy_value = r.u64();
  } else if (kind == "LcOpToolElement") {
    auto &v = b.value.emplace<ToolState>();
    v.saved_tool = r.u32();
    v.effective_tool = version < 119 && !v.saved_tool ? none : v.saved_tool;
    if (version >= 408 && v.saved_tool == 700)
      v.plugin_name = r.string();
  } else if (kind == "LcOpGraphicsSystemElement") {
    b.value.emplace<GraphicsSystemState>().saved_value = r.u32();
  } else if (kind == "LcOpXRefTable") {
    b.value = read_xref_table(r, options);
  } else if (kind == "LcOpCurrentViewElement" ||
             kind == "LcOpHomeViewElement" || kind == "LcOpPlanViewElement" ||
             kind == "LcOpSectionViewElement") {
    b.value = read_current_view(r, version);
  } else if (kind == "LcOpBackgroundElement") {
    auto &v = b.value.emplace<Background>();
    if (version >= 110)
      v.mode = r.read<int32_t>();
    v.colors.resize(version < 110 ? 1 : version < 112 ? 4 : 7);
    for (auto &c : v.colors)
      for (auto &x : c)
        x = r.read<double>();
    ObjectReader objects(r.data, v.objects, version, options);
    if (version >= 214)
      v.paper_style = objects.object(r);
    if (version >= 252)
      v.asset = objects.object(r);
  } else if (kind == "LcTlSimulateTimeElement" ||
             kind == "LcTlSimulateDurationElement") {
    std::optional<int32_t> module;
    if (version < 241) {
      module = r.read<int32_t>();
      if (*module > 8)
        throw Unsupported("TimeLiner simulation module version");
    }
    if (kind == "LcTlSimulateTimeElement") {
      auto &v = b.value.emplace<TimeLinerClock>();
      v.module_version = module;
      v.time = version < 90 ? r.read<int32_t>() : r.read<int64_t>();
    } else {
      auto &v = b.value.emplace<TimeLinerSimulation>();
      v.module_version = module;
      int m = module.value_or(8);
      auto time = [&](uint32_t boundary) {
        v.times.push_back(version < boundary ? r.read<int32_t>()
                                             : r.read<int64_t>());
      };
      auto integer = [&] { v.integers.push_back(r.read<int32_t>()); };
      auto flag = [&] { v.booleans.push_back(boolean(r)); };
      auto string = [&] { v.strings.push_back(r.string()); };
      time(233);
      integer();
      time(233);
      integer();
      if (m >= 4) {
        flag();
        string();
      }
      if (m >= 6)
        integer();
      if (m >= 7) {
        flag();
        if (version >= 56)
          integer();
        if (version >= 85) {
          string();
          integer();
          integer();
          time(0);
          time(0);
          flag();
          flag();
        }
        if (version >= 89)
          flag();
        if (version >= 95) {
          integer();
          v.animation_path = string_pair(r);
        }
      }
    }
  } else if (kind == "LcTlAppearanceDefinitionsElement" ||
             kind == "LcTlTaskTypeDefinitionsElement" ||
             kind == "LcTlDefaultStatusElement") {
    auto &v = b.value.emplace<LegacyTimeLinerDefinitions>();
    if (version < 241) {
      v.module_version = r.read<int32_t>();
      if (*v.module_version > 8)
        throw Unsupported("legacy TimeLiner definitions module version");
    }
    auto state = [&] {
      LegacyTimeLinerState s;
      s.flag = boolean(r);
      for (auto &x : s.parameters)
        x = r.read<int32_t>();
      return s;
    };
    if (kind == "LcTlDefaultStatusElement")
      v.default_status = state();
    else if (kind == "LcTlAppearanceDefinitionsElement") {
      for (auto n = count(r, options.max_objects); n; --n) {
        auto &a = v.appearances.emplace_back();
        a.name = r.string();
        for (auto &x : a.color)
          x = r.read<int32_t>();
        a.opacity = r.read<double>();
      }
    } else {
      for (auto n = count(r, options.max_objects); n; --n) {
        auto &t = v.task_types.emplace_back();
        t.name = r.string();
        t.legacy_name = r.string();
        t.states.push_back(state());
        t.states.push_back(state());
        t.flag = boolean(r);
        if (version >= 56) {
          t.states.push_back(state());
          t.states.push_back(state());
        }
        if (version >= 85)
          t.states.push_back(state());
      }
    }
  } else if (kind == "LcOdpDBDatabaseLinksElement") {
    auto &v = b.value.emplace<DatabaseLinks>();
    ObjectReader objects(r.data, v.objects, version, options);
    for (auto n = count(r, options.max_objects); n; --n) {
      auto &link = v.links.emplace_back();
      link.name = objects.object(r);
      require(link.name == none || v.objects.objects.at(link.name).type == 52,
              "database link name is not a name object");
      link.tagged_sql = r.string();
      auto size = r.u32();
      auto bytes = r.raw(size);
      link.encoded_connection.assign(bytes.begin(), bytes.end());
      link.hold_open = boolean(r);
      link.active = boolean(r);
      for (auto c = count(r, options.max_objects); c; --c) {
        auto &f = link.fields.emplace_back();
        f.field = r.string();
        f.display = r.string();
      }
    }
    // Preserve every record before attempting configuration decoding.
    for (auto &link : v.links)
      link.tagged_connection =
          decode_database_connection(link.encoded_connection);
  } else if (kind == "LcOpNwdStats") {
    b.value.emplace<SceneStatistics>().text = r.string();
  } else if (kind == "LcOpGuidStore") {
    auto &v = b.value.emplace<GuidStore>();
    v.present = boolean(r);
    if (v.present) {
      auto n = count(r, options.max_objects);
      // GUIDs have a fixed 16-byte layout with no inter-entry padding.
      // Validate before allocating and copy the complete array once.
      require(n <= (r.data.size() - r.pos) / 16, "truncated GUID store");
      auto bytes = r.raw(size_t(n) * 16);
      v.guids.resize(n);
      if (n)
        std::memcpy(v.guids.data(), bytes.data(), bytes.size());
    }
  } else if (kind == "LcOpGridElement") {
    auto &v = b.value.emplace<Grids>();
    uint64_t remaining = options.max_objects;
    auto grid_count = [&] {
      auto n = count(r, remaining);
      remaining -= n;
      return n;
    };
    for (auto n = grid_count(); n; --n) {
      auto &s = v.systems.emplace_back();
      s.label = r.string();
      for (auto &x : s.frame)
        x = r.read<double>();
      for (auto c = grid_count(); c; --c) {
        auto &l = s.lines.emplace_back();
        l.label = r.string();
        for (auto &x : l.parameters)
          x = r.read<double>();
        for (auto &x : l.flags)
          x = r.byte();
        for (auto k = grid_count(); k; --k) {
          auto &segment = l.segments.emplace_back();
          segment.type = r.u32();
          if (segment.type != 5 && segment.type != 2)
            throw Unsupported("grid segment type " +
                              std::to_string(segment.type));
          for (unsigned i = 0; i < (segment.type == 5 ? 2u : 3u); ++i) {
            auto &point = segment.points.emplace_back();
            for (auto &x : point)
              x = r.read<double>();
          }
        }
      }
      for (auto c = grid_count(); c; --c) {
        auto &level = s.levels.emplace_back();
        level.label = r.string();
        level.elevation = r.read<double>();
      }
      if (version >= 401)
        s.locked_level = r.read<int32_t>();
    }
    if (version >= 401) {
      v.active_system = r.read<int32_t>();
      v.render_mode = r.read<int32_t>();
    }
  } else if (kind == "LcTlGUISettingsElement") {
    auto &v = b.value.emplace<TimeLinerGui>();
    if (version < 241) {
      v.module_version = r.read<int32_t>();
      if (*v.module_version > 8)
        throw Unsupported("TimeLiner GUI module version");
    }
    for (auto n = count(r, options.max_objects); n; --n) {
      auto name = r.string();
      auto value = r.read<int32_t>();
      v.columns.emplace_back(std::move(name), value);
    }
    if (version <= 234) {
      v.legacy_value = r.read<int32_t>();
      for (auto n = count(r, options.max_objects); n; --n) {
        std::array<int32_t, 2> pair;
        for (auto &x : pair)
          x = r.read<int32_t>();
        v.legacy_pairs.push_back(pair);
      }
    }
  } else if (kind == "LcOpHeadlightElement") {
    auto &v = b.value.emplace<Headlight>();
    v.ambient = r.read<double>();
    v.secondary_value = r.read<double>();
  } else if (kind == "LcOpCullingElement") {
    auto &v = b.value.emplace<Culling>();
    v.area_cull_threshold = r.read<double>();
    if (version >= 105)
      v.near_mode = r.u32();
    v.near_distance = r.read<double>();
    if (version >= 105)
      v.far_mode = r.u32();
    v.far_distance = r.read<double>();
    v.backface_mode = r.u32();
  } else if (kind == "LcOpSpeedElement") {
    b.value = NavigationSpeed{r.read<double>()};
  } else if (kind == "LcOpNextCommentIdElement") {
    auto &v = b.value.emplace<CommentIds>();
    v.next = r.u64();
    if (version >= 86)
      v.second_counter = r.u64();
  } else {
    auto &v = b.value.emplace<SavedItems>();
    v.root_count =
        kind == "LcOpCurrentAnimationElement" || kind == "LcOpOdyFileInfoChunk"
            ? 1
            : count(r, options.max_objects);
    ObjectReader objects(r.data, v.objects, version, options);
    items(r, v, none, v.root_count, version, schemas, options, objects, 0, 0,
          kind == "LcOpCurrentAnimationElement" ? 1
          : kind == "LcOpOdyFileInfoChunk"      ? 70
                                                : none);
    if (kind == "LcOpClashElement") {
      for (auto n = count(r, options.max_objects); n; --n) {
        auto name = r.string();
        auto value = r.read<int32_t>();
        v.ignore_plugins.emplace_back(std::move(name), value);
      }
      read_source_references(r, v.source_references, objects, version, options);
    }
  }
}
} // namespace
ProductData Document::read_products() const {
  ProductData out;
  out.version = version();
  out.chunks = chunks();
  out.blocks.resize(chunks().size());
  auto schema_count =
      std::count_if(chunks().begin(), chunks().end(), [](const Chunk &c) {
        return c.name == "LcOpCommonSchemas";
      });
  for (size_t i = 0; i < chunks().size(); ++i)
    if (chunks()[i].name == "LcOpCommonSchemas") {
      auto &b = out.blocks[i];
      if (schema_count > 1) {
        b.status = ProductStatus::failed;
        b.diagnostic = "duplicate common schema table";
        continue;
      }
      std::vector<uint8_t> bytes;
      try {
        bytes = read_chunk(i);
        b.decoded_bytes = bytes.size();
        out.schemas = decode_schemas(bytes, version());
        b.consumed_bytes = bytes.size();
        b.status = ProductStatus::decoded;
      } catch (const Error &e) {
        b.status = ProductStatus::failed;
        b.diagnostic = e.what();
        b.unparsed_tail = std::move(bytes);
      }
    }
  detail::parallel_for(chunks().size(), options().threads, [&](size_t i) {
    const auto &c = chunks()[i];
    auto kind = std::string_view(c.name);
    if (auto pos = kind.rfind('\\'); pos != kind.npos)
      kind.remove_prefix(pos + 1);
    auto &b = out.blocks[i];
    if (!supported(kind))
      return;
    const bool fixed_cipher =
        c.flags == 3 &&
        (kind == "LcOpNwdSerial" || kind == "LcOpNwdGeometryCompress");
    if ((c.flags > 1 && !fixed_cipher) ||
        ((c.prefix_bytes || c.index_bytes) &&
         kind != "LcOpFileDatabaseElementNWD")) {
      b.diagnostic = "unsupported product chunk envelope";
      return;
    }
    std::vector<uint8_t> bytes;
    try {
      bytes = read_product_payload(i);
    } catch (const Error &e) {
      b.status = ProductStatus::failed;
      b.diagnostic = e.what();
      return;
    }
    b.decoded_bytes = bytes.size();
    detail::Cursor r(bytes, c.name);
    try {
      const bool implicit_node_map =
          std::any_of(chunks().begin(), chunks().end(), [](const Chunk &chunk) {
            return chunk.name.ends_with("LcOpNwfSceneSet");
          });
      decode(b, r, kind, version(), out.schemas, options(), implicit_node_map);
      r.exact();
      b.status = ProductStatus::decoded;
      if (auto *v = std::get_if<CurrentView>(&b.value))
        v->chunk_name = c.name;
    } catch (const Unsupported &e) {
      b.status = ProductStatus::partial;
      b.diagnostic = e.what();
    } catch (const Error &e) {
      b.status = ProductStatus::failed;
      b.diagnostic = e.what();
    }
    b.consumed_bytes = r.pos;
    if (b.status != ProductStatus::decoded)
      b.unparsed_tail.assign(bytes.begin() + r.pos, bytes.end());
  });
  return out;
}
} // namespace nwd
