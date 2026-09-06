#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <variant>
#include <optional>
namespace nwd {
using Id = uint32_t;
inline constexpr Id none = UINT32_MAX;
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Options {
  unsigned threads = 0;
  unsigned normal_bits = 0; // 0: inspect supported precision candidates, with
                            // complete record validation.
  bool metadata = true;
  bool viewpoints =
      false; // optional product data must not block core model processing
  bool resources = false; // load embedded image/LWI bytes only on request
  bool products =
      false; // read additional document records with per-block status
  bool intern_transforms = true; // exact representation comparison, never
                                 // approximate spatial merging.
  uint64_t max_decoded_chunk = 2ull << 30;
  uint64_t max_objects = 20000000;
};
struct Chunk {
  std::string name;
  uint32_t flags = 0, prefix_bytes = 0, index_bytes = 0;
  uint64_t offset = 0, size = 0;
};
struct Reference {
  Id graph = none, object = none;
};
struct Value {
  uint32_t tag = 0;
  Id string = none;
  Reference reference;
  int64_t integer = 0;
  std::array<double, 3> number{};
};
struct Property {
  Id name = none;
  Value value;
};
struct ShaderArgument {
  Id name = none;
  Value value;
};
struct Shader {
  Id name = none;
  uint32_t slot = 0;
  std::vector<ShaderArgument> arguments;
};
struct ProteinProperty {
  uint32_t type = 0, flags = 0, count = 0;
  Id name = none;
  bool enabled = false;
  std::vector<Reference> connections;
  std::vector<Id> strings;
  std::vector<int32_t> integers;
  std::vector<double> numbers;
  std::vector<uint8_t> bytes;
  std::vector<Id> embedded_files; // owning ObjectGraph.embedded_files indices
};
// Immutable arenas: object IDs are local to each graph, never guessed across
// pages.
struct Object {
  uint32_t type = 0, flags = 0;
  Id name = none, class_name = none;
  uint64_t stream_offset = 0;
  std::vector<Id> children, strings, integers;
  std::vector<Reference> attributes, references;
  std::vector<double> numbers;
  std::vector<uint64_t> wide;
  std::vector<Property> properties;
  std::vector<ProteinProperty> protein_properties;
  std::vector<Shader> shaders;
  std::vector<uint8_t> bytes;
};
struct EmbeddedAssetFile {
  Id owner = none; // graph-local object ID; none for Asset::embedded_files
  uint32_t ordinal =
      0; // original file-list position, including external entries
  std::string name, prefix, suffix;
  std::vector<uint8_t> bytes;
};
struct ObjectGraph {
  std::vector<std::string> strings;
  std::vector<Object> objects;
  std::vector<Id> roots;
  std::vector<EmbeddedAssetFile>
      embedded_files; // sparse arena, no per-object overhead
};
struct Path {
  Id parent = none, object = none;
};
struct AttributeArray {
  bool raw = false;
  bool finite =
      true; // invalid source UV ranges are retained, never silently repaired
  std::array<float, 4> quantization_bounds{};
  std::array<uint8_t, 4> component_bits{};
  uint32_t type = 0, flag = 0, bits = 0, palette_count = 0;
  std::vector<uint8_t> packed_palette;
  std::vector<int32_t> indices;
};
struct SchemaValue {
  uint32_t type =
      20; // 0 double, 1 GUID, 2 int32, 3 bool, 4 string, 20 struct, 21 vector
  double number = 0;
  int32_t integer = 0;
  std::string text;
  std::array<uint8_t, 16> guid{};
  std::vector<SchemaValue> children; // ordered fields or vector elements
};
struct SchemaField {
  uint32_t type = 20, qualifier = 0;
  std::string name, display_name, qualifier_name;
  std::vector<std::string> concepts;
  SchemaValue default_value;
  std::vector<SchemaField> children;
};
struct SchemaDefinition {
  std::vector<std::string> names;
  SchemaField root;
};
std::vector<SchemaDefinition> decode_schemas(std::span<const uint8_t>,
                                             uint32_t version);
struct ExternalGeometry {
  std::string loader, format, source_path;
  uint64_t payload_record_offset = 0;
  std::vector<uint8_t> unparsed_payload; // legacy raw payload, retained even
                                         // when payload_decoded
  bool payload_decoded = false;
  Id schema = none; // index into Scene.schemas
  SchemaValue properties;
  uint32_t flags = 0, flags2 = 0, geometry_kind = 0;
  std::array<float, 6> bounds{};
};
struct Geometry {
  uint32_t type = 0, flags = 0;
  std::string text;
  Id text_style = none; // index into the model's shared-node table
  std::array<float, 13> parameters{};
  std::vector<float> coordinates;
  std::vector<uint32_t> coordinate_indices, strip_lengths, strip_indices;
  std::vector<AttributeArray> attributes;
  std::shared_ptr<const ExternalGeometry>
      external; // descriptor, not an embedded mesh
};
struct Transform {
  uint32_t type = 0;
  std::array<double, 16> values{};
};
struct GeometryReference {
  std::array<float, 6> bounds{};
  std::array<double, 3> origin{};
  float tolerance = 0;
  uint32_t checksum = 0;
  Id geometry = none;
};
struct Material {
  std::array<float, 14> values{};
  std::span<const float, 3> ambient() const {
    return std::span<const float, 3>(values.data(), 3);
  }
  std::span<const float, 3> diffuse() const {
    return std::span<const float, 3>(values.data() + 3, 3);
  }
  std::span<const float, 3> specular() const {
    return std::span<const float, 3>(values.data() + 6, 3);
  }
  std::span<const float, 3> emissive() const {
    return std::span<const float, 3>(values.data() + 9, 3);
  }
  float shininess() const { return values[12]; }
  float transparency() const { return values[13]; }
};
struct Asset {
  std::string json, extra;
  std::vector<std::pair<std::string, std::string>> files;
  std::vector<EmbeddedAssetFile> embedded_files;
};
// Values retain source JSON types; connection IDs preserve the material graph.
struct AssetParameter {
  std::string category, name, values_json;
  std::vector<std::string> connections;
  bool connections_enabled = true;
};
struct AssetNode {
  std::string id, definition, name;
  std::vector<AssetParameter> parameters;
};
struct AssetUri {
  std::string node, property, value;
  bool thumbnail = false;
};
struct AssetDescription {
  std::vector<std::string> roots;
  std::vector<AssetNode> nodes;
  std::vector<AssetUri> uris;
};
AssetDescription describe_asset(const Asset &);
struct Appearance {
  uint32_t flags = 0;
  Id material = none, asset = none;
  std::array<uint32_t, 4> overrides{};
};
struct Instance {
  Id path = none, geometry_reference = none, transform = none,
     appearance = none, auxiliary_transform = none;
  uint32_t flags = 0, primitive_count = 0, bits = 0;
  double precision = 0;
};
struct AuxiliaryTransform {
  uint32_t type =
      0; // 0 affine + orientation, 1 translation, 2 translation + XYZW
  std::array<double, 12> values{}; // native order, unused entries zero
  bool orientation = false;        // serialized only for type 0
};
struct Model {
  std::string name;
  std::vector<Id> schema_references; // Scene.schemas indices; none for null
  std::vector<Geometry> geometries;
  std::vector<GeometryReference> geometry_references;
  std::vector<Transform> transforms;
  std::vector<AuxiliaryTransform> auxiliary_transforms;
  std::vector<Material> materials;
  std::vector<Asset> assets;
  std::vector<Appearance> appearances;
  std::vector<Instance>
      instances; // serialized fragment slots; repeated slots retained
  std::vector<ObjectGraph>
      graphs; // 0 partition, 1 hierarchy, 2+ property pages
  std::vector<Path> paths;
  ObjectGraph
      shared_nodes; // text styles and other shared resources, own ID space
  uint64_t property_attribute_count = 0, source_transform_count = 0;
  int linear_units = -1;
  double meters_per_unit = 0;
  bool raw_coordinates = false;
  unsigned normal_bits = 0;
};
struct Timing {
  double container_ms = 0, geometry_ms = 0, instances_ms = 0, metadata_ms = 0,
         viewpoints_ms = 0, resources_ms = 0, total_ms = 0;
};
struct Camera {
  uint32_t projection = 0;
  std::array<double, 3> position{};
  std::array<double, 4> orientation{}; // XYZW
  // Aspect, height/angle, near, far, then version >=425 lens fields.
  std::array<double, 6> parameters{};
};
// Secondary fields retain their serialization order; see docs/FORMAT.md.
struct ViewFields {
  std::vector<uint32_t> integers;
  std::vector<double> numbers;
  std::vector<std::string> strings;
};
struct CurrentView {
  std::string chunk_name;
  uint32_t parts = 0;
  Camera camera;
  ViewFields viewer, state, clip_set;
  std::vector<ViewFields> clip_planes;
};
struct Background {
  int32_t mode = 0;
  std::vector<std::array<double, 3>> colors; // serialized palette order
  ObjectGraph objects;
  Id paper_style = none, asset = none; // identities in objects
};
struct Headlight {
  double ambient = 0, secondary_value = 0;
};
struct Culling {
  double area_cull_threshold = 0, near_distance = 0, far_distance = 0;
  // Modes are absent in versions before 105; do not invent stored values.
  std::optional<uint32_t> near_mode, far_mode, backface_mode;
};
struct NavigationSpeed {
  double value = 0;
};
struct CommentIds {
  uint64_t next = 0;
  std::optional<uint64_t> second_counter;
};
struct SavedComment {
  std::string author, text;
  int64_t timestamp = 0; // source stream time, without timezone conversion
  uint64_t id = 0;
  int32_t status = 0;
};
struct SchemaInstance {
  Id schema = none;
  SchemaValue value;
};
struct SearchCondition {
  Id category = none, property = none; // SavedItems.objects name objects
  uint32_t condition = 0, options = 0;
  Value value; // strings and graph=0 objects belong to SavedItems.objects
};
struct SavedSelection {
  uint32_t kind =
      0; // implicit-selection mode; unused for plain find-selection records
  // Stream path identities, not ObjectGraph IDs. Use the owning SavedItems
  // model only after its explicit partition associations are verified.
  std::vector<uint32_t> path_links;
  std::vector<std::pair<std::string, std::string>> item_paths;
  uint32_t item_path_mode = 0;
  std::vector<SearchCondition> conditions;
  std::string locator; // original find-selection expression
  uint32_t search_mode = 0;
  bool prune_below_match = false;
  std::string source;
  int64_t timestamp = 0;
};
enum class RedlineType : uint32_t { line, ellipse, cloud, tag, text, arrow };
struct Redline {
  RedlineType type = RedlineType::line;
  std::vector<std::array<double, 2>> points;
  std::string text;
  int32_t width = 0;
  std::array<double, 3> color{};
  std::optional<uint16_t> style; // source pattern, version >=409
  std::array<uint64_t, 2> tag_ids{};
  uint32_t tag_extra = 0;
  bool tag_position_flag = false, tag_bounds_flag = false;
  std::array<double, 3> tag_position{};
  std::array<double, 6> tag_bounds{};
};
struct AnimationKeyFrame {
  double time = 0;
  bool interpolate = false;
  uint32_t flags = 0;
  std::optional<std::array<double, 3>> translation, center, color, scale_size;
  // Native rotation component order is retained without normalization.
  std::optional<std::array<double, 4>> rotation, tool_orientation,
      scale_orientation;
  std::optional<double> opacity, focal_distance;
  std::optional<Camera> camera;
  std::vector<ViewFields> clip_planes;
  ViewFields clip_set;
};
struct TimeLinerStatus {
  uint32_t mode = 0;
  std::string appearance;
};
struct TimeLinerTaskType {
  std::array<TimeLinerStatus, 5> states;
}; // start,end,underrun,overrun,simulation-start
struct TimeLinerAppearance {
  std::array<double, 3> color{};
  double opacity = 0;
};
struct TimeLinerTask {
  bool enabled = false;
  std::string synchronization_id, display_id, task_type, data_source;
  std::array<int64_t, 2> actual_dates{},
      planned_dates{}; // start,end; original stream time
  // enabled, start-date flag, end-date flag
  std::array<bool, 3> actual_flags{}, planned_flags{};
  std::optional<SavedSelection> implicit_selection, find_selection;
  std::optional<CurrentView> legacy_view;  // viewpoint only; no clip set
  std::vector<SavedComment> task_comments; // separate serialized list
  std::vector<std::string> user_data;
  uint32_t animation_behavior = 0;
  std::pair<std::string, std::string> animation_path, script_path;
  bool progress_flag = false;
  double progress_percent = 0;
  // material, labor, equipment, subcontractor
  std::array<double, 4> costs{};
  std::array<bool, 4> cost_flags{};
  std::optional<std::pair<std::string, double>> legacy_cost;
};
struct TimeLinerCsv {
  std::array<std::string, 4> fields; // source strings in serialized order
  bool first_row_header = false, custom_date_format = false;
  std::string date_format;
  std::optional<int32_t> row_count;
};
struct TimeLinerDataSource {
  std::string provider_id, provider_name, project;
  double provider_version = 0;
  int64_t sync_time = 0;
  std::vector<std::pair<std::string, std::string>> available_fields,
      user_fields;
  // type,synchronization-ID,planned-start,planned-end,actual-start,actual-end,
  // material-cost,labor-cost,equipment-cost,subcontractor-cost
  std::vector<std::pair<std::string, std::string>> field_mappings;
  std::optional<TimeLinerCsv> csv;
};
using TimeLinerValue =
    std::variant<std::monostate, TimeLinerTask, TimeLinerTaskType,
                 TimeLinerAppearance, TimeLinerStatus, TimeLinerDataSource>;
struct ProductRule {
  std::string plugin, name;
  bool enabled = false;
  std::vector<std::pair<int32_t, Value>>
      parameters; // values use SavedItems.objects
};
struct Assignee {
  std::string name;
  std::optional<std::string> id;
};
struct ClashTest {
  uint32_t type = 0, status = 0, simulation_type = 0;
  std::string custom_test;
  std::array<SavedSelection, 2> selections;
  std::array<bool, 2> self_intersect{};
  std::array<uint32_t, 2> primitive_flags{};
  double tolerance = 0, simulation_step = 0;
  std::pair<std::string, std::string> animation_path;
  std::vector<ProductRule> rules;
  bool relative_touch = false;
  double relative_tolerance = 0, absolute_tolerance = 0;
  std::optional<int64_t> run_time;
  std::optional<bool> merge_composites;
  std::optional<int32_t> priority;
  std::optional<Assignee> assignee;
};
struct ClashSimulationEvent {
  uint32_t type = 0;
  std::array<int64_t, 2> times{};
  double time = 0;
  std::string name;
  std::array<std::string, 2> task_names;
  std::array<std::pair<std::string, std::string>, 2> item_paths;
};
struct ClashResult {
  std::string test_name;
  std::optional<int32_t> priority;
  double distance = 0;
  std::array<uint32_t, 2> path_links{none, none};
  std::optional<std::array<uint32_t, 2>> fallback_path_links;
  std::array<std::array<double, 3>, 2> points{}, bounds{};
  int64_t created_time = 0;
  uint32_t status = 0;
  std::optional<uint32_t> run_test_type;
  std::optional<int64_t> approved_time, resolved_time;
  std::optional<Assignee> approved_by, assigned_to, resolved_by;
  ClashSimulationEvent simulation;
};
struct ClashResultGroup {
  std::string test_name; // serialized from version 450
  std::optional<int32_t> priority;
};
using ClashValue =
    std::variant<std::monostate, ClashTest, ClashResult, ClashResultGroup>;
struct SavedSheetInfo {
  std::string sheet_id;
  uint32_t sheet_type = 0;
  std::vector<Id> property_categories; // SavedItems.objects
  std::optional<std::string> initial_file, initial_sheet;
  std::optional<std::string> legacy_file_and_sheet;
  std::optional<std::string> initial_sheet_display_name;
  std::optional<std::array<uint8_t, 16>> source_guid;
};
struct SavedFileInfo {
  std::string default_sheet_id;
  std::vector<Id> property_categories; // SavedItems.objects
  std::optional<std::array<uint8_t, 16>> source_guid, file_version_guid;
  std::vector<Id>
      default_sheet_matches; // SavedItems.items; preserves ambiguity
};
struct SavedLight {
  Id object =
      none; // SavedItems.objects, type 27; asset reference in that object
  std::array<double, 3> position{}, target{};
};
struct SavedItem {
  Id material_asset = none; // type 80; SavedItems.objects, type 185
  std::optional<SavedSheetInfo> sheet_info;
  std::optional<SavedFileInfo> file_info;
  std::optional<SavedLight> light;
  uint32_t type = 0;
  Id parent = none;
  uint64_t offset = 0, end_offset = 0; // decoded chunk offsets
  bool complete = false;
  std::string name;
  std::vector<SavedComment> comments;
  std::array<uint8_t, 16> guid{};
  std::vector<SchemaInstance> properties;
  std::optional<CurrentView> view;
  std::optional<double> cut_duration;
  uint32_t animation_flags = 0, animation_mode = 0;
  std::optional<double> end_time;
  bool infinite = false;
  std::optional<AnimationKeyFrame> keyframe;
  TimeLinerValue timeliner;
  ClashValue clash;
  uint32_t child_list =
      0; // distinguishes the two serialized lists in animation folders
  std::optional<SavedSelection> selection;
  uint32_t redline_count = 0, element_record_count = 0;
  std::vector<Id> element_records; // SavedItems.objects, type 89
  std::vector<Redline> redlines;
};
struct CachedReference {
  std::string name, path;
  uint64_t timestamp = 0, size = 0;
};
struct CacheOption {
  std::string name;
  Value value; // strings and graph=0 object references: NwfData.option_values
               // or SavedItems.objects
  uint32_t flags = 0; // nested option-set flags when value.tag == 0
  std::vector<CacheOption> children;
};
struct CachePlugin {
  std::string name;
  int32_t version = 0;
  bool has_options = false;
  CacheOption options; // root option set; unnamed
};
struct NwfReference {
  std::string name, original_path, partition, display_name, plugin, extra;
  std::array<uint8_t, 16> source_guid{}, reference_guid{};
  uint32_t flags = 0, load_flags = 0, linear_units = 0, angular_units = 0,
           orientation_flag = 0, extra_enum = 0;
  std::array<double, 12> affine{}; // native row-major 3x3, then translation
  std::array<double, 3> up{}, front{}, north{};
  std::vector<CachedReference> cached_files;
  std::vector<CachePlugin> cache_plugins; // primary plugin first
  std::vector<CacheOption> cache_options;
};
struct SavedItems {
  bool legacy_clash =
      false; // types mapped to 50/51/52; result status retains old enum
  std::vector<std::pair<std::string, int32_t>> ignore_plugins;
  std::vector<NwfReference> source_references; // cache values use objects below
  uint32_t root_count = 0;
  std::vector<SavedItem> items; // preorder; parent is an index in this vector
  ObjectGraph objects; // shared name/variant objects across the entire block
  // Applies only to serialized PathLink fields, including element records.
  // Named locators, searches, GUIDs and item-path strings are not evaluated.
  Id model = none;
  bool associations_verified = false, implicit_node_map = false;
};
struct TimeLinerGui {
  std::optional<int32_t> module_version;
  std::vector<std::pair<std::string, int32_t>> columns;
  std::optional<int32_t> legacy_value;
  std::vector<std::array<int32_t, 2>> legacy_pairs;
};
struct TimeLinerClock {
  std::optional<int32_t> module_version;
  int64_t time = 0;
};
struct TimeLinerSimulation {
  std::optional<int32_t> module_version;
  // Each array retains fields of its type in serialized order. Enum meanings
  // and time units are not normalized; see the format version and module
  // version.
  std::vector<int64_t> times;
  std::vector<int32_t> integers;
  std::vector<bool> booleans;
  std::vector<std::string> strings;
  std::optional<std::pair<std::string, std::string>> animation_path;
};
struct LegacyTimeLinerState {
  bool flag = false;
  // Serialized parameters, without applying native runtime conversion.
  std::array<int32_t, 3> parameters{};
};
struct LegacyTimeLinerAppearance {
  std::string name;
  std::array<int32_t, 3> color{};
  double opacity = 0;
};
struct LegacyTimeLinerTaskType {
  std::string name, legacy_name;
  // Source order; older versions omit the final states.
  std::vector<LegacyTimeLinerState> states;
  bool flag = false;
};
struct LegacyTimeLinerDefinitions {
  std::optional<int32_t> module_version;
  // The containing chunk identifies which definition collection is present.
  std::vector<LegacyTimeLinerAppearance> appearances;
  std::vector<LegacyTimeLinerTaskType> task_types;
  std::optional<LegacyTimeLinerState> default_status;
};
enum class TextureMapping : uint32_t {
  box,
  plane,
  cylinder,
  sphere,
  explicit_uv
};
struct TextureSpace {
  TextureMapping mapping = TextureMapping::explicit_uv;
  bool parameters_present = false; // before version 428 only mapping is stored
  // Serialized vectors: two common vectors, then two type-specific vectors
  // for box, plane and cylinder. Their semantic axes are not normalized.
  std::vector<std::array<double, 3>> vectors;
  std::array<double, 4> rotation{}; // source XYZW quaternion
  bool cylinder_flag = false;
  double cylinder_cap_threshold = 0;
  std::vector<uint32_t> directions; // box: 6 pairs; cylinder: 4 enums
};
struct NwfTextureSpace {
  bool node_scope =
      false; // shared node identity, otherwise exact path identity
  std::vector<Id>
      paths; // NWF selectors; legacy node lookup may contain candidates
  TextureSpace value;
};
struct TextureSpaceOverrides {
  bool implicit_node_map = false;
  std::vector<NwfTextureSpace> records;
  Id model = none; // read_scene(): exact source model namespace
  bool associations_verified = false;
};
enum class ProductStatus { not_handled, decoded, partial, failed };
struct DatabaseFieldMapping {
  std::string field, display;
};
struct DatabaseLink {
  Id name = none; // DatabaseLinks.objects, type 52 name object
  std::string tagged_sql;
  std::vector<uint8_t>
      encoded_connection; // exact saved bytes, retained on failure
  std::optional<std::string> tagged_connection; // UTF-8; no macro expansion
  bool hold_open = false, active = false;
  std::vector<DatabaseFieldMapping> fields;
};
struct DatabaseLinks {
  std::vector<DatabaseLink> links;
  ObjectGraph objects; // shared name identities across the complete block
};
struct GuidStore {
  bool present = false;
  // Serialized order is significant; duplicates are retained.
  std::vector<std::array<uint8_t, 16>> guids;
};
struct GridSegment {
  uint32_t type = 0; // 5: straight line; 2: circular arc
  // Line: start/end. Arc: center/start/end, in the system's 2D frame.
  std::vector<std::array<double, 2>> points;
};
struct GridLine {
  std::string label;
  std::array<double, 2> parameters{}; // serialized line extent parameters
  std::array<uint8_t, 2> flags{};
  std::vector<GridSegment> segments;
};
struct GridLevel {
  std::string label;
  double elevation = 0;
};
struct GridSystem {
  std::string label;
  std::array<double, 12> frame{}; // four source 3-vectors; no normalization
  std::vector<GridLine> lines;
  std::vector<GridLevel> levels;
  std::optional<int32_t> locked_level;
};
struct Grids {
  std::vector<GridSystem> systems;
  std::optional<int32_t> active_system, render_mode;
};
struct SceneStatistics {
  std::string text; // saved statistics report, with original line breaks
};
struct SceneProperties {
  std::string saved_filename;
  std::optional<std::array<uint8_t, 16>> guid;
  uint64_t legacy_value = 0; // retained field; native reader discards it
};
struct GeometryCompression {
  uint32_t flags = 0;
  uint8_t normal_precision = 0, color_precision = 0,
          texture_coordinate_precision = 0;
  float coordinate_precision = 0;
};
struct FileSerial {
  std::string value;
};
struct ToolState {
  uint32_t saved_tool = 0, effective_tool = 0;
  std::optional<std::string> plugin_name; // tool 700 from version 408
};
struct GraphicsSystemState {
  uint32_t saved_value = 0; // native reader consumes but ignores this value
};
struct ExternalReferenceTable {
  std::string json, saved_filename;
  std::vector<std::pair<std::string, std::string>> path_remaps;
};
using DatabaseCell = std::variant<std::monostate, int64_t, double, std::string,
                                  std::vector<uint8_t>>;
struct DatabaseForeignKey {
  int64_t id = 0, sequence = 0;
  std::string table, from, on_update, on_delete, match;
  std::optional<std::string> to; // null means the referenced primary key
};
struct DatabaseTable {
  std::string name;
  std::vector<std::string> columns;
  std::vector<std::vector<DatabaseCell>> rows;
  std::vector<DatabaseForeignKey> foreign_keys;
};
struct DatabaseSchemaEntry {
  std::string type, name, table;
  std::optional<std::string> sql; // null for automatic indexes
  int64_t root_page = 0;
};
struct FileDatabase {
  std::string prefix, suffix;
  uint32_t page_size = 0, page_count = 0, user_version = 0;
  std::vector<DatabaseSchemaEntry> schema;
  std::vector<DatabaseTable> tables;
};
struct Hyperlink {
  std::string url, label;
  Id category = none; // HyperlinkOverrides.objects, type 52
  std::vector<std::array<double, 3>> positions;
};
struct HyperlinkOverride {
  uint32_t path_link = none;
  std::vector<Hyperlink> links;
};
struct NodeHyperlinkOverride {
  std::vector<Id> paths; // explicit PathLink or implicit node candidates
  std::vector<Hyperlink> links;
};
struct HyperlinkOverrides {
  std::vector<HyperlinkOverride> paths;
  ObjectGraph objects;
  std::vector<NodeHyperlinkOverride> nodes;
  bool implicit_node_map = false;
  Id model = none;
  bool associations_verified = false;
};
struct NodeOverride {
  uint32_t path_link = none, flags = 0;
};
struct NodeScopedOverride {
  std::vector<Id> paths; // node identity, not one specific occurrence
  uint32_t flags = 0;
};
struct NodeOverrides {
  std::vector<NodeOverride> paths;
  std::vector<NodeScopedOverride> nodes;
  bool implicit_node_map = false;
  // Version >= 437: low byte is the override mask, high byte the values.
  // effective = (base & ~mask) | (values & mask). Older words remain raw.
  bool mask_value_encoding = false;
  Id model = none;
  bool associations_verified = false;
};
struct CacheMetadata {
  int32_t version = 0;
  uint32_t flags = 0;
  std::string source_filename, source_sheet;
  int64_t source_timestamp = 0;
  uint64_t source_size = 0;
  std::vector<CachePlugin> plugins;
  std::vector<CachedReference> references;
  std::vector<CacheOption> options;
  bool saved_state = false;
  ObjectGraph objects;
};
struct SpatialNode {
  uint32_t type = 0; // 0 null, 1 shape, 2 static leaf, 3 static group, 4 single
                     // leaf, 5 dynamic group
  Id parent = none, first_child = none, next_sibling = none;
  Id fragment = none; // source fragment slot for type 1/4
};
struct SpatialHierarchy {
  std::vector<SpatialNode>
      nodes;       // preorder, compact adjacency; no geometry copies
  Id model = none; // owning Scene.models index, filled by read_scene()
  bool associations_verified = false;
};
struct PublishInformation {
  Id name = none, class_name = none; // objects below
  uint32_t attribute_flags = 0, flags = 0;
  std::string title, subject, author, publisher, copyright, published_for,
      comments, keywords;
  int64_t published = 0, expires = 0;
  ObjectGraph objects;
};
// LightWorks values retain their wire type separately from the C++ storage.
using LightWorksValue =
    std::variant<uint32_t, double, std::string, std::vector<uint8_t>,
                 std::vector<uint32_t>, std::vector<double>,
                 std::vector<std::string>>;
struct LightWorksField {
  uint32_t id = 0, type = 0;
  std::string name; // empty when the file and known schema supply no name
  LightWorksValue value;
};
struct LightWorksFieldDefinition {
  uint32_t id = 0, type = 0;
  std::string name;
};
struct LightWorksDefinition {
  uint32_t type = 0, version = 0;
  std::string name;
  bool stored = false;
  std::vector<LightWorksFieldDefinition> fields;
};
struct LightWorksObject {
  uint32_t type = 0, flags = 0, header_flags = 0;
  std::optional<uint32_t> identity;
  std::optional<std::array<uint8_t, 4>> extension;
  Id parent = none, first_child = none, next_sibling = none;
  Id reference = none; // same archive's object index; no copied object payload
  bool null_reference = false;
  std::vector<LightWorksField> fields;
};
struct LightWorksArchive {
  uint32_t engine_version = 0, encoding = 0, flags = 0, block_bits = 0;
  uint32_t compression = 0, cipher = 0, stream_flags = 0;
  std::string key_name;
  std::vector<uint8_t> cipher_parameters;
  std::vector<LightWorksDefinition> definitions;
  std::vector<LightWorksObject> objects;
  uint32_t frame_count =
      0; // decoded linked pages; object fields may span pages
  uint32_t header_value = none, root_page = 0;
  std::vector<uint8_t> encoded_key; // encoding0 key bytes, before restoration
  std::vector<Id> page_order;       // logical traversal of physical pages
};
struct LightWorksImage {
  uint32_t width = 0, height = 0, bits_per_pixel = 0;
  std::vector<uint8_t> pixels; // packed source bytes; no color conversion
};
// On-demand codec1 (raw) or codec2 (zlib) image decoding. Pass a resolved
// LtImage object, not an identity-reference record. Source fields stay intact.
LightWorksImage decode_lightworks_image(const LightWorksObject &,
                                        uint64_t max_bytes = 256ull << 20);
struct LegacyShaderArgument {
  std::string name;
  uint32_t type = 0;
  LightWorksValue value;
};
struct LegacyShader {
  std::string name;
  std::vector<LegacyShaderArgument> arguments;
};
struct PresenterEntity {
  uint32_t mode = 0;
  Id archive = none; // PresenterData.archives / PresenterLights.archives
  std::string legacy_material_name;
  std::optional<LegacyShader> legacy_shader;
  std::array<std::optional<LegacyShader>, 5> legacy_material_shaders;
};
struct PresenterBinding {
  bool node_scope = false;
  std::vector<uint32_t> paths; // exact link or node candidate IDs, file-local
  Id material = none;          // index in PresenterData.materials
};
struct PresenterTextureBinding {
  bool node_scope = false;
  std::vector<uint32_t> paths;
  uint32_t mapping = 0;
  std::optional<PresenterEntity> entity;
};
struct PresenterData {
  std::optional<bool> enabled;
  bool implicit_node_map = false;
  PresenterEntity background;
  std::vector<PresenterEntity> materials;
  std::vector<PresenterBinding> material_bindings;
  std::vector<PresenterTextureBinding> texture_bindings;
  std::vector<LightWorksArchive> archives;
  Id model = none; // Scene.models index, assigned by read_scene()
  bool associations_verified = false; // selectors checked against Model.paths
};
struct PresenterLights {
  std::vector<PresenterEntity> lights;
  std::vector<LightWorksArchive> archives;
};
using ProductValue = std::variant<
    std::monostate, CurrentView, Background, Headlight, Culling,
    NavigationSpeed, CommentIds, SavedItems, TimeLinerGui, TimeLinerClock,
    TimeLinerSimulation, LegacyTimeLinerDefinitions, DatabaseLinks, GuidStore,
    Grids, SceneStatistics, SceneProperties, ToolState, GraphicsSystemState,
    ExternalReferenceTable, FileDatabase, HyperlinkOverrides,
    GeometryCompression, FileSerial, NodeOverrides, PublishInformation,
    CacheMetadata, TextureSpaceOverrides, SpatialHierarchy, PresenterData,
    PresenterLights>;
struct ProductBlock {
  ProductStatus status = ProductStatus::not_handled;
  uint64_t decoded_bytes = 0, consumed_bytes = 0;
  ProductValue value;
  std::string diagnostic;
  // Only attempted incomplete blocks retain their tail. Opaque directory
  // entries can be retrieved explicitly with Document::read_chunk().
  std::vector<uint8_t> unparsed_tail;
};
struct ProductData {
  uint32_t version = 0;
  std::vector<Chunk> chunks;
  std::vector<SchemaDefinition> schemas;
  std::vector<ProductBlock> blocks; // one per Document::chunks() entry
};
struct EmbeddedResource {
  std::string name;
  uint32_t block_size_hint = 0;
  std::vector<uint8_t> bytes; // original encoded file; no image transcoding
};
struct Scene {
  uint32_t version = 0;
  std::string header;
  std::vector<Chunk> chunks;
  std::vector<bool>
      parsed_chunks; // one per directory entry; false means opaque/unparsed
  std::vector<Model> models;
  std::vector<SchemaDefinition> schemas;
  std::vector<CurrentView> current_views;
  std::vector<EmbeddedResource> resources;
  std::shared_ptr<const ProductData> products;
  Timing timing;
  std::vector<std::string> warnings;
};
struct NwfPath {
  Id parent = none;
  uint32_t flags = 0, type = 0;
  int32_t sibling = -1;
  std::string class_name, name, source;
  std::array<uint8_t, 16> guid{};
  uint64_t entity = 0;
  std::vector<uint32_t> fragment_checksums;
  std::vector<double> bounds;
};
struct AppearanceSelection {
  Id appearance = none;
  std::vector<Id> paths; // NWF path-map IDs, not model path IDs
};
struct NwfTransformOverride {
  Id path = none; // NWF path-map ID
  uint32_t flags = 0;
  // Native row-vector order: identity=0 values, linear=9, translation=3,
  // affine=12, projective=16. Project applies validated direct-source
  // overrides.
  std::vector<double> values;
};
struct NwfData {
  std::string path_map_namespace;  // directory prefix owning the implicit map
  std::vector<bool> parsed_chunks; // Document directory coverage; standalone
                                   // decoders leave empty
  ObjectGraph option_values;
  std::vector<NwfReference> references;
  uint32_t linear_units = 0, angular_units = 0, path_map_kind = 0,
           path_map_flags = 0;
  std::array<double, 3> up{}, front{}, north{};
  std::vector<NwfPath>
      paths;               // 0 null, 1 implicit root, 2+ serialized selectors
  Model appearance_values; // shared appearances/materials/assets only
  std::vector<AppearanceSelection> appearance_overrides;
  std::vector<NwfTransformOverride> transform_overrides;
  std::vector<NwfTextureSpace> texture_spaces;
  Id global_asset = none;
  std::string saved_filename, xref_json;
  std::vector<std::pair<std::string, std::string>> path_remaps;
  std::vector<std::string> unparsed_core;
};
NwfData decode_nwf_scene_set(std::span<const uint8_t>, uint32_t version);
void decode_nwf_path_map(NwfData &, std::span<const uint8_t>);
void decode_nwf_transforms(NwfData &, std::span<const uint8_t>);
void decode_nwf_texture_spaces(NwfData &, std::span<const uint8_t>,
                               uint32_t version);
class Document {
  struct Impl;
  std::shared_ptr<Impl> impl_;
  std::vector<uint8_t> read_product_payload(size_t index) const;

public:
  explicit Document(const std::filesystem::path &path, Options options = {});
  uint32_t version() const;
  const std::string &header() const;
  const std::vector<Chunk> &chunks() const;
  std::vector<uint8_t> read_chunk(size_t index) const;
  EmbeddedResource read_resource(size_t index) const;
  Scene read_scene() const;
  NwfData read_nwf() const;
  ProductData
  read_products() const; // also works for NWF without embedded geometry
  const Options &options() const;
};
struct ProjectOptions {
  Options reader;
  std::vector<std::filesystem::path> search_paths;
  std::vector<std::pair<std::string, std::filesystem::path>> remaps;
  bool load_textures = true;
  uint32_t max_reference_depth = 32;
};
struct ProjectSource {
  std::filesystem::path path;
  uint32_t version = 0;
  std::shared_ptr<const Scene> scene;
  std::shared_ptr<const NwfData> nwf;
  std::shared_ptr<const ProductData> products;
};
struct ProjectNode {
  Id parent = none, source = none, model = none, reference = none;
  std::string name, requested_path, status;
  std::array<double, 16> to_parent{1, 0, 0, 0, 0, 1, 0, 0,
                                   0, 0, 1, 0, 0, 0, 0, 1}; // meters to meters
  bool placement_supported = true;
  bool orientation_changed = false; // source/reference orientation hint differs
};
struct ProjectAppearance {
  Id source = none, model = none,
     appearance = none; // model=none: NWF appearance_values
};
struct AppliedAppearance {
  Id node = none, instance = none;
  ProjectAppearance appearance;
};
struct AppliedTransform {
  Id owner = none, node = none, instance = none;
  std::array<double, 16> matrix{}; // meters; applied in the owner NWF frame
};
struct TextureSpaceAssignment {
  Id owner = none, node = none, path = none, record = none;
  // record indexes the owning NWF's texture_spaces. A node-scope record also
  // addresses other occurrences of the same source graph/object identity.
};
enum class ProductBindingKind {
  presenter_material,
  presenter_texture,
  hyperlink,
  node_override,
  texture_space
};
enum class ProductBindingStatus { resolved, empty, unresolved };
struct ProductBinding {
  Id owner = none; // Project.nodes NWF occurrence; its source owns the block
  Id block = none, record = none;
  ProductBindingKind kind = ProductBindingKind::node_override;
  bool node_scope = false;
  ProductBindingStatus status = ProductBindingStatus::unresolved;
  Id node = none, path = none; // target ProjectNode and source Model.paths
};
struct SavedPathBinding {
  Id owner = none, block = none, path_link = none;
  ProductBindingStatus status = ProductBindingStatus::unresolved;
  Id node = none, path = none;
  // A resolved NWF implicit root addresses the owner's project subtree,
  // rather than an arbitrary referenced model's path0.
  bool project_root = false;
};
struct SelectionLocatorPath {
  std::string root;
  std::vector<std::string> parts;
};
struct SelectionLocator {
  bool select_all = false;
  std::vector<SelectionLocatorPath> paths; // source order; union of targets
};
SelectionLocator parse_selection_locator(std::string_view,
                                         uint64_t max_parts = 1000000);
enum class SelectionTargetStatus {
  resolved,
  tree_root,
  missing,
  ambiguous,
  unsupported_root,
  unavailable
};
struct SelectionTarget {
  SelectionTargetStatus status = SelectionTargetStatus::missing;
  Id block = none, item = none; // ProductData.blocks / SavedItems.items
};
struct SelectionLocatorResult {
  bool select_all = false;
  std::vector<SelectionTarget> targets; // one per locator path, same order
};
// Reusable name index. Owns its names/indices, not model or selection payloads.
// Resolves saved-item identities; it does not evaluate their search conditions.
class SelectionSetIndex {
  struct Impl;
  std::shared_ptr<const Impl> impl_;

public:
  explicit SelectionSetIndex(const ProductData &,
                             uint64_t max_items = 10000000);
  SelectionLocatorResult resolve(std::string_view owner_chunk,
                                 const SelectionLocator &) const;
};
struct TextureFile {
  Id source = none, model = none, asset = none;
  std::string alias, requested_path, status;
  std::filesystem::path resolved_path;
  std::shared_ptr<const std::vector<uint8_t>>
      bytes; // aliasing view for embedded files
  bool active = false, thumbnail = false;
};
struct Project {
  std::vector<ProjectSource> sources; // each canonical file parsed once
  std::vector<ProjectNode> nodes; // each reference occurrence stays distinct
  std::vector<AppliedAppearance> appearance_overrides; // sorted (node,instance)
  std::vector<AppliedTransform>
      transform_overrides; // sorted (owner,node,instance)
  std::vector<TextureSpaceAssignment> texture_space_assignments;
  std::vector<ProductBinding> product_bindings; // NWF product selectors
  std::vector<TextureFile> textures;
  std::vector<std::string> warnings;
  bool complete = true;
  // One entry per distinct source identity, sorted by (owner,block,path_link).
  // SavedItems keep the original repeated IDs and field positions.
  std::vector<SavedPathBinding> saved_path_bindings;
};
const SavedPathBinding *saved_path_binding(const Project &, Id owner, Id block,
                                           Id path_link);
Project load_project(const std::filesystem::path &, ProjectOptions = {});
std::array<double, 16> model_base_matrix(const Model &);
std::array<double, 16> nwf_transform_matrix(const NwfTransformOverride &);
// Replaces the stored base transform. Throws for unvalidated unit overrides.
std::array<double, 16> reference_placement_matrix(const Model &,
                                                  const NwfReference &,
                                                  uint32_t parent_units);
ProjectAppearance effective_appearance(const Project &, Id node, Id instance);
const Model &appearance_arena(const Project &, ProjectAppearance);
std::array<double, 16> project_world_matrix(const Project &, Id node,
                                            Id instance); // output meters
Geometry decode_geometry(std::span<const uint8_t> data,
                         unsigned normal_bits = 8, bool raw_coordinates = false,
                         bool raw_strips = false);
CurrentView decode_current_view(std::span<const uint8_t> data,
                                uint32_t version);
std::vector<uint32_t> triangle_indices(const Geometry &geometry);
// On-demand slot decoding: XYZ normals, RGBA colors, or UV in the first lanes.
// Short normals divide by 32767; values are not clamped or renormalized.
// Invalid source UV ranges remain non-finite. No expanded vertex arrays are
// cached.
std::array<float, 4> attribute_value(const AttributeArray &attribute,
                                     size_t slot);
std::array<double, 16> world_matrix(const Model &model,
                                    const Instance &instance);
// Indices for repeated data-processing queries. Built once, no geometry
// copying. Rebuild after changing model.paths or model.instances. IDs are
// model-local.
class ModelIndex {
  std::vector<Id> child_offsets_, children_, instance_offsets_, instances_;

public:
  explicit ModelIndex(const Model &model);
  std::span<const Id> children(Id path) const;
  std::span<const Id> instances(Id path) const;
  size_t storage_bytes() const;
};
// References point into the model. No string/property materialization occurs.
const Object &path_object(const Model &model, Id path);
Reference path_reference(const Model &model,
                         Id path); // root resolves to partition graph
const Object &resolve_object(const Model &model, Reference reference);
std::string_view resolve_string(const ObjectGraph &graph, Id string);
} // namespace nwd
