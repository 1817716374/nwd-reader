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
  std::vector<Instance> instances;
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
// Secondary fields retain their source serialization order.
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
  Timing timing;
  std::vector<std::string> warnings;
};
struct CachedReference {
  std::string name, path;
  uint64_t timestamp = 0, size = 0;
};
struct CacheOption {
  std::string name;
  Value value; // strings and graph=0 object references: NwfData.option_values
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
struct NwfData {
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

public:
  explicit Document(const std::filesystem::path &path, Options options = {});
  uint32_t version() const;
  const std::string &header() const;
  const std::vector<Chunk> &chunks() const;
  std::vector<uint8_t> read_chunk(size_t index) const;
  EmbeddedResource read_resource(size_t index) const;
  Scene read_scene() const;
  NwfData read_nwf() const;
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
  std::vector<TextureFile> textures;
  std::vector<std::string> warnings;
  bool complete = true;
};
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
