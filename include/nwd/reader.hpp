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
#include <functional>
namespace nwd {
using Id = uint32_t;
inline constexpr Id none = UINT32_MAX;
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Options {
  unsigned threads = 0;
  unsigned normal_bits =
      0; // 0: use saved model compression; if absent,
         // inspect candidates with complete record validation.
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
  // Tags14/16 retain unsigned integer bits without increasing Value's size.
  uint64_t unsigned_integer() const noexcept {
    return static_cast<uint64_t>(integer);
  }
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
struct PartitionOrientation {
  std::array<double, 3> up{}, north{};
  // Not serialized before wire version109. A present zero vector is retained.
  std::optional<std::array<double, 3>> front;
};
// Interpret a decoded type32 partition (including an inline partition).
// Numeric layout from version53 onward; whole-file compatibility is separate.
// Use the containing Scene/Document version, not the product's marketing year.
// No normalization, generated defaults or reference/SceneSet overrides. Values
// including nonfinite numbers are preserved. The hierarchy's synthetic root
// has no partition payload and is rejected, as are inconsistent field arrays.
// Fixed-size result, no allocation; use path_object(model,0) for the file root.
PartitionOrientation partition_orientation_fields(const Object &,
                                                  uint32_t wire_version);
struct EmbeddedAssetFile {
  Id owner = none; // graph-local object ID; none for Asset::embedded_files
  uint32_t ordinal =
      0; // original file-list position, including external entries
  std::string name, prefix, suffix;
  std::vector<uint8_t> bytes;
};
struct AnimationObject;
struct LegacyNodeRecord {
  Id owner = none;
  Reference modified;                      // version0 NodeModified18; nullable
  std::optional<Reference> group_modified; // groups only, GroupModified19
};
// Raw extra records in wire0..27 partitions. Kept outside Object so modern
// metadata objects incur no per-object storage cost. Spatial objects use their
// own type1/2 child lists, separate from a partition's logical children.
struct LegacyPartitionRecord {
  Id owner = none;
  Reference spatial_root;
  std::vector<Reference> shapes; // original order, including null references
  uint32_t flag = 0; // raw serialized boolean; semantic name not established
  // Derived stream position immediately before the spatial root. At the
  // stream's root partition this is when native logical links become ready.
  std::optional<uint64_t> path_links_ready_offset;
  bool shapes_present = false; // array and flag saved only in versions1..27
  // Versions2..21 save two strings instead of the common scene header.
  // The first also supplies Object.name. Native loading discards the second
  // and generates a fixed class name; no Name object is serialized here.
  std::optional<std::array<Id, 2>> legacy_names;
};
struct GeometryObjectArena;
struct ObjectGraph {
  std::vector<std::string> strings;
  std::vector<Object> objects;
  std::vector<Id> roots;
  std::vector<EmbeddedAssetFile>
      embedded_files; // sparse arena, no per-object overhead
  std::vector<AnimationObject> animation_objects; // sparse, ordered by owner ID
  std::vector<LegacyPartitionRecord> legacy_partitions; // associate by owner ID
  std::shared_ptr<GeometryObjectArena>
      geometry_arena;                         // absent in ordinary graphs
  std::vector<LegacyNodeRecord> legacy_nodes; // version0, associate by owner ID
};
struct Path {
  Id parent = none, object = none;
};
struct AttributeArray {
  bool raw = false;
  bool finite = true; // nonfinite source values/decoded results are retained,
                      // not repaired
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
  std::array<bool, 3> null_strings{}; // loader, format, saved source path
};
struct CoordinateQuantization {
  float saved_precision = 0;
  std::array<float, 6> bounds{}; // lower XYZ, upper XYZ, without repair
  std::array<uint8_t, 4>
      component_bits{}; // fourth byte retained, unused in XYZ
  uint32_t palette_count = 0;
  std::vector<uint8_t> packed_palette; // little-endian words, MSB-first fields
  bool finite = true; // saved floats and decoded coordinates are all finite
};
struct TextGeometryFields {
  uint32_t parameter_count = 0; // 8 before wire version103, otherwise12
  bool uses_shared_nodes = false;
  int32_t shared_node_index =
      -1;                 // saved signed value, including invalid values
  Reference inline_style; // graph-local raw reference, even for a wrong class
  // Only a standalone page with inline objects owns a separate graph here.
  // Model graph objects leave this empty, so ownership cannot form a cycle.
  std::shared_ptr<const ObjectGraph> record_graph;
  bool text_is_null = false; // distinguish a saved null string from empty text
};
struct Geometry {
  uint32_t type = 0, flags = 0;
  std::string text;
  Id text_style = none; // raw shared-table index bits; inline uses text_fields
  std::array<float, 13> parameters{};
  std::vector<float> coordinates;
  std::vector<uint32_t> coordinate_indices, strip_lengths, strip_indices;
  std::vector<AttributeArray> attributes;
  std::shared_ptr<const ExternalGeometry>
      external; // descriptor, not an embedded mesh
  std::shared_ptr<const CoordinateQuantization> coordinate_quantization;
  std::shared_ptr<const TextGeometryFields> text_fields;
};
struct GeometrySpecialObject {
  Id owner = none;
  Geometry data; // text103, circle104, cylinder105 or external181 descriptor
};
// Shared object records keep graph-local identity; no copied per-primitive
// coordinate or attribute arrays. Owners and references index ObjectGraph.
struct GeometryCoordinateObject {
  Id owner = none;
  std::vector<float> values;
  std::vector<uint32_t> indices;
  std::shared_ptr<const CoordinateQuantization> quantization;
};
struct GeometryAttributeObject {
  Id owner = none;
  AttributeArray data;
};
struct GeometryPrimitiveObject {
  Id owner = none, coordinates = none;
  uint32_t type = 0, flags = 0, vertex_count = 0;
  std::array<Id, 3> attributes{none, none,
                               none}; // serialized UV, normal, color
  std::vector<uint32_t> lengths, indices;
  bool implicit_indices = true;
};
struct GeometryReferenceObject;
struct GeometryShapeObject;
struct GeometryAuxiliaryObject;
struct GeometryLegacyShapeFields {
  Id owner = none;
  std::array<double, 6> initial_bounds{}, geometry_bounds{};
  double geometry_tolerance = 0;
  uint32_t geometry_present_word = 0;
};
struct LegacyMaterialObject;
struct LegacyAppearanceObject;
struct LegacyGeometryObject {
  Id owner = none;
  std::array<uint32_t, 5> enums{}; // four header enums, then trailing enum
  int32_t saved_integer = 0;
  std::array<Id, 8> references{none, none, none, none, none, none, none, none};
  std::vector<int32_t>
      saved_lengths; // retain values before native uint16 narrowing
  GeometryPrimitiveObject
      primitive; // implicit, shares graph arrays; not a new wire object
};
struct GeometryObjectArena {
  // Each sparse table is ordered by owner. Graph references retain identity
  // even when numeric payloads happen to be equal.
  std::vector<GeometryCoordinateObject> coordinates;
  std::vector<GeometryAttributeObject> attributes;
  std::vector<GeometryPrimitiveObject> primitives;
  std::vector<GeometryReferenceObject> references;
  std::vector<GeometryShapeObject> shapes;
  std::vector<GeometryAuxiliaryObject> auxiliary_transforms;
  std::vector<GeometryLegacyShapeFields> legacy_shapes;
  std::vector<LegacyMaterialObject> legacy_materials;
  std::vector<LegacyAppearanceObject> legacy_appearances;
  std::vector<LegacyGeometryObject> legacy_geometries;
  std::vector<GeometrySpecialObject> special;
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
struct GeometryReferenceObject {
  Id owner = none, inline_geometry = none;
  uint32_t data_id = 0; // raw page-record identity, never a graph object ID
  bool uses_store = false, store_range_checked = false;
  bool user_fields_present = false, checksum_present = false;
  GeometryReference fields; // geometry stays none until a Model table is bound
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
struct LegacyMaterialObject {
  Id owner = none;
  float saved_prefix = 0;
  std::array<int16_t, 3> saved_modes{};
  Material fields;
};
struct ProteinAssetReference {
  Id graph = none, object = none; // graph indexes Model.graphs when no arena
  std::shared_ptr<const ObjectGraph>
      record_graph; // fragment-local shared arena
};
struct Asset {
  std::string json, extra;
  std::vector<std::pair<std::string, std::string>> files;
  std::vector<EmbeddedAssetFile> embedded_files;
  std::optional<ProteinAssetReference> protein; // pre443 original type182 graph
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
struct ProteinConnection {
  Id owner = none, property = none, ordinal = none, target = none;
  Id native_ordinal = none; // null saved entries are absent from native vector
  bool enabled = false; // property connection flag, not literal-value enable
};
struct ProteinUriSlot {
  Id owner = none, property = none, ordinal = none;
  Id string = none, embedded_file = none;
};
struct ProteinReachableAsset {
  Id object = none;
  bool enabled_path = false; // at least one path of enabled connections
};
struct ProteinAssetFields {
  Id library_id = none, definition_id = none;
  std::optional<Reference> schema; // absent before254; present may be null
  std::optional<std::array<uint8_t, 16>> guid; // saved bytes only, from411
  std::optional<std::span<const Id>> thumbnails, categories,
      keywords;                                               // from415
  std::optional<Id> ui_name, default_preview_id, description; // 417/429/430
};
struct ProteinSchemaFields {
  Id identifier = none, asset_type = none;
  std::span<const uint8_t> data;
  Reference parent;
};
// Borrow the immutable graph. IDs refer to its strings/objects; optional Id
// distinguishes absent fields from a saved null string (present, value none).
ProteinAssetFields protein_asset_fields(const ObjectGraph &, Id owner,
                                        uint32_t wire_version, Id graph_id = 0);
ProteinSchemaFields protein_schema_fields(const ObjectGraph &, Id owner,
                                          Id graph_id = 0);
enum class ProteinSchemaNodeKind { element, text, comment, instruction };
struct ProteinSchemaXmlAttribute {
  std::string name, value;
};
struct ProteinSchemaXmlNode {
  ProteinSchemaNodeKind kind = ProteinSchemaNodeKind::element;
  Id parent = none, first_child = none, next_sibling = none;
  uint64_t byte_offset = 0; // original XML byte offset, including BOM
  std::string name, value; // decoded UTF-8; text/PI/comment use value
  std::vector<ProteinSchemaXmlAttribute> attributes; // original order
};
struct ProteinSchemaXmlOptions {
  uint64_t max_bytes = 16ull << 20, max_text_bytes = 64ull << 20;
  uint32_t max_nodes = 1000000, max_attributes = 1000000, max_depth = 256;
};
struct ProteinSchemaEntryFields {
  std::string_view kind;
  std::optional<std::string_view> id, value, type, label, unit, alias, file;
  std::array<std::optional<std::string_view>, 4> rgba;
  std::array<std::optional<std::string_view>, 3> xyz;
  // Schema declaration flags, not ProteinProperty::flags or active values.
  // public=1, readonly=2, multiple values=4, definition iterator data=8,
  // local=16, single/multiple connections=32/64, metadata=2048.
  uint32_t declaration_flags = 1;
};
enum class ProteinSchemaNumberStatus {
  defaulted, exact, prefix, empty, invalid, out_of_range
};
struct ProteinSchemaNumber {
  std::string text; // unescaped token, or unsplit constraint attribute
  std::optional<double> value;
  ProteinSchemaNumberStatus status = ProteinSchemaNumberStatus::defaulted;
  uint64_t consumed = 0; // bytes through decimal prefix, including leading space
};
struct ProteinSchemaValueOptions {
  uint32_t max_values = 1000000; // total tokens + normalized components per call
};
struct ProteinSchemaDefaults {
  std::string_view effective_id; // borrows document, except fixed "assettype"
  std::vector<std::string> tokens; // val split on unescaped ',' or '|'
  std::optional<ProteinSchemaNumber> scalar; // Float/Distance or int32 Integer
  std::optional<bool> boolean_value, texture_reference;
  // Color: RGBA, equal lengths with 0/0/0/1 padding. Vector3: XYZ, length 1.
  // valX/Y/Z alias valR/G/B; X/Y/Z win if both aliases are present.
  std::array<std::vector<ProteinSchemaNumber>, 4> components;
};
struct ProteinSchemaConstraintFields {
  std::optional<ProteinSchemaNumber> minimum, maximum;
  ProteinSchemaNumber increment; // 1 when incrementsize is absent
  bool minimum_inclusive = true, maximum_inclusive = true, percentage = false;
};
enum class ProteinSchemaConstraintStatus { absent, ready, discarded, unresolved };
struct ProteinSchemaNumericRange {
  std::optional<double> minimum, maximum;
  double increment = 1;
  bool minimum_inclusive = true, maximum_inclusive = true, percentage = false;
};
struct ProteinSchemaResolvedConstraint {
  Id source = none; // last numeric-constraint candidate in this declaration
  ProteinSchemaConstraintStatus status = ProteinSchemaConstraintStatus::absent;
  std::optional<ProteinSchemaNumericRange> range; // present only when ready
  // Cached final candidate fields, including unresolved conversions. Empty
  // only when absent; preserves inactive-bound storage and comparison flags.
  std::shared_ptr<const ProteinSchemaConstraintFields> fields;
};
enum class ProteinSchemaNumericStatus { ready, rejected, unresolved, unsupported };
struct ProteinSchemaNumericDefaults {
  ProteinSchemaNumericStatus status = ProteinSchemaNumericStatus::unsupported;
  uint32_t value_type = 0, components = 0; // declaration code; 1, 3 (XYZ), 4 (RGBA)
  bool multiple = false;
  // Only scalar Float/Distance consumes a nonempty unit. No conversion is
  // applied. unit_code is the native code for a known length name. Other names
  // need the unresolved unit-provider lookup; they do not become code zero.
  std::string unit;
  std::optional<uint32_t> unit_code;
  // Bits: 1=first value outside range, 2=fractional Integer bounds,
  // 4=first Color outside RGB [0,255] or alpha [0,1].
  uint32_t rejections = 0;
  // Only Integer/Float/Distance consult a numeric constraint. An unresolved
  // increment alone does not prevent the value projection being ready.
  ProteinSchemaResolvedConstraint constraint;
  // Row-major candidate components, complete/accepted only when ready.
  // Rejected/unsupported projections have no candidates. Unresolved numeric
  // text keeps its conversion status; it never becomes a fabricated zero.
  std::vector<ProteinSchemaNumber> candidates;
};
enum class ProteinSchemaTextStatus { ready, rejected, unsupported };
struct ProteinSchemaTextDefaults {
  ProteinSchemaTextStatus status = ProteinSchemaTextStatus::unsupported;
  uint32_t value_type = 0; // factory type: String=2, DateTime=3, URL=4, Uuid=11
  uint32_t flags = 0; // after XML declaration preprocessing
  bool multiple = false;
  // A scalar without a val token leaves its newly constructed value untouched.
  // Supported scalar text factories construct an empty internal string.
  // This marker distinguishes keeping that value from assigning an empty
  // token; values below still describe explicit factory assignments only.
  bool uses_type_default = false;
  // Explicit text assignments in factory order. Scalar: at most one. Array:
  // all tokens, including duplicates/empty tokens. Owns the returned strings.
  std::vector<std::string> values;
};
enum class ProteinSchemaConstructedKind {
  unsupported, empty_binary, empty_reference_array, empty_texture_link
};
struct ProteinSchemaConstructedDefaults {
  ProteinSchemaConstructedKind kind = ProteinSchemaConstructedKind::unsupported;
  uint32_t flags = 0; // XML-preprocessed declaration flags
};
enum class ProteinSchemaIdentityKind { unsupported, uid, base_schema };
enum class ProteinSchemaIdentityStatus {
  unsupported, ready, invalid_value, redefined
};
struct ProteinSchemaIdentityAssignment {
  ProteinSchemaIdentityKind kind = ProteinSchemaIdentityKind::unsupported;
  ProteinSchemaIdentityStatus status = ProteinSchemaIdentityStatus::unsupported;
  std::string value; // owns the assigned token; ready UID can assign empty text
  // A ready Base assigns a String property with ID BaseSchema and flags8.
  // UID assigns definition metadata, not a property, and leaves flags zero.
  uint32_t flags = 0;
};
// Schema XML declaration type codes (not ProteinProperty's wire discriminant):
// binary=1, string=2, datetime=3, URL=4, boolean=5, int32=6, real=7,
// color=8, texture=9, choice=10, UUID=11, reference=12. Exact case required.
// Mapping a name does not assert a supported factory or inherited definition.
std::optional<uint32_t> protein_schema_value_type(std::string_view) noexcept;
// Native declaration flag validation, before type-specific flag rewriting.
// Conflict bits: 1=flags outside 12 bits, 2=readonly + definition iterator,
// 4=unsupported multiple values, 8=distance on non-real type,
// 16=UI/implementation/texture-URI role on non-URL type,
// 32=both connection modes, or a texture/reference with neither mode.
// Zero only means no flag conflict; IDs, aliases and factories are separate.
uint32_t protein_schema_flag_conflicts(uint32_t value_type,
                                      uint32_t declaration_flags) noexcept;
struct ProteinSchemaPreparedFields {
  std::string_view label; // borrows fields.label or effective_id
  uint32_t flags = 0;
};
// XML declaration preprocessing after ID/flag validation, before its factory.
// Supply the working ID (defaults().effective_id for ordinary declarations).
// Raw fields remain unchanged. Base/UID/PropertyAlias handlers, acceptance,
// property values and inheritance are separate; this is not a validity check.
// Applies type roles and the native exact-case length-unit flag, no conversion.
ProteinSchemaPreparedFields protein_schema_prepare_fields(
    const ProteinSchemaEntryFields &, std::string_view effective_id) noexcept;
// On-demand XML decoding of type183 data. Owns immutable decoded strings and
// ordered nodes; copies share storage, independent reads are thread-safe.
// Preserves unknown declarations/attributes, duplicate IDs, choice children,
// text, comments and processing instructions. A valid AssetSchema root is
// required. DTDs are explicitly unsupported; no external resources are read.
// XML names and values are case-sensitive. Raw fields are never normalized.
// Optional default/constraint accessors interpret finite decimal values without
// locale or shared mutable state; status distinguishes missing defaults, numeric
// prefixes and unresolved numeric input (empty/invalid/out_of_range => no value).
// This does not emulate every MSVC stream failure/overflow edge, resolve inherited
// defaults or library identities, apply constraints, or create runtime materials.
class ProteinSchemaDocument {
  struct Impl;
  std::shared_ptr<const Impl> impl;

public:
  explicit ProteinSchemaDocument(std::span<const uint8_t>,
                                  ProteinSchemaXmlOptions = {});
  std::span<const ProteinSchemaXmlNode> nodes() const;
  Id root() const;
  std::span<const Id> entries() const; // immediate element children of root
  ProteinSchemaEntryFields entry_fields(Id) const;
  ProteinSchemaDefaults defaults(Id, ProteinSchemaValueOptions = {}) const;
  // Nested elements visited by the native numeric-constraint reader, in source
  // order. ChoiceValue descendants of a Choice declaration use another reader.
  // Keep all candidates: this is not a resolved runtime constraint selection.
  std::vector<Id> constraint_nodes(Id entry) const;
  ProteinSchemaConstraintFields constraint_fields(Id node) const;
  // Selects the last candidate, including a rejected candidate that clears a
  // preceding range. Percentage forces inclusive [0,1] with increment 1.
  // Unresolved numeric conversion stays explicit. This resolves the local
  // constraint object, not factory acceptance or inherited property values.
  ProteinSchemaResolvedConstraint resolved_constraint(Id entry) const;
  // XML value factory phase for Integer, Float, Distance, Color and Vector3.
  // Checks the first value before array conversion, preserves empty arrays,
  // and applies Vector3's scalar rewrite. This does not validate declaration
  // flags/IDs, merge inherited fields or register the resulting property.
  ProteinSchemaNumericDefaults numeric_defaults(Id entry,
                                                ProteinSchemaValueOptions = {}) const;
  // XML text assignments for String, DateTime, Uuid, URL, TextureURI, ui,
  // implementation, type, localtype and localname. DateTime rejects multiple;
  // Uuid and the three special string factories always select one token.
  // type/localtype/localname reject an absent or empty original val, before
  // unescaping. Their ID rewriting is available through defaults().effective_id.
  // UI/implementation preserve raw val as one string. No URL normalization,
  // UUID decoding or date parsing occurs in this factory phase. Ready does not
  // validate declaration flags/IDs or downstream formats and registrations.
  // All input tokens count against max_values, including ignored scalar tails.
  ProteinSchemaTextDefaults text_defaults(Id entry,
                                         ProteinSchemaValueOptions = {}) const;
  // Binary and Reference factories leave their newly constructed data intact:
  // empty bytes, an empty connection array, or a default texture symbol link.
  // Reference type="texture" selects the latter (exact case). val does not
  // supply bytes or target identities. Its tokens still count against the
  // budget, without materializing ignored strings. This describes local XML
  // factory defaults, not declaration validity, inherited values or graph links.
  ProteinSchemaConstructedDefaults constructed_defaults(
      Id entry, ProteinSchemaValueOptions = {}) const;
  // Local UID/Base assignment gate against the caller's current value. Both
  // require exactly one token. UID tests the original val for nonemptiness;
  // Base tests the decoded token. Invalid value precedes redefinition checks.
  // A nonempty current_value rejects redefinition. Does not update state,
  // resolve aliases/inheritance, load Base's file or register a definition.
  // All input tokens count against max_values; only the first is materialized.
  ProteinSchemaIdentityAssignment identity_assignment(
      Id entry, std::string_view current_value = {},
      ProteinSchemaValueOptions = {}) const;
};
enum class ProteinSchemaChoiceStatus {
  accepted,
  empty_id,
  negative_value,
  duplicate_id,
  duplicate_value,
  unresolved_attributes,
  unresolved_value,
  unresolved_previous
};
struct ProteinSchemaChoiceEntry {
  Id source = none;
  std::optional<std::string_view> id, label, raw_value;
  ProteinSchemaNumber value;
  ProteinSchemaChoiceStatus status = ProteinSchemaChoiceStatus::unresolved_value;
  Id conflict = none; // earlier accepted entry index, not an XML node ID
};
enum class ProteinSchemaChoiceLookupStatus { found, missing, unresolved };
struct ProteinSchemaChoiceLookup {
  ProteinSchemaChoiceLookupStatus status = ProteinSchemaChoiceLookupStatus::missing;
  Id entry = none; // index into entries(), present only when found
};
enum class ProteinSchemaChoiceDefaultStatus { ready, missing_first, unresolved };
enum class ProteinSchemaChoiceTokenStatus { matched, missing, unresolved, ignored };
struct ProteinSchemaChoiceDefaultToken {
  std::string text;
  ProteinSchemaChoiceTokenStatus status = ProteinSchemaChoiceTokenStatus::ignored;
  Id entry = none; // Choice index entry, present only when matched
};
struct ProteinSchemaChoiceDefaults {
  ProteinSchemaChoiceDefaultStatus status = ProteinSchemaChoiceDefaultStatus::missing_first;
  bool multiple = false;
  std::vector<ProteinSchemaChoiceDefaultToken> tokens;
  std::vector<int32_t> values; // complete values only when status is ready
};
// Builds one immutable Choice declaration table; owns a shared document copy.
// Preserves all ChoiceValue nodes in preorder, including rejected/uncertain ones.
// Nonempty IDs and nonnegative int32 values must both be unique; first accepted
// entries win. Uncertain preceding entries cannot silently become absent.
// O(n log n) construction and O(log n) allocation-free ID lookup. Independent
// reads are thread-safe. This does not assert factory/default-value acceptance.
class ProteinSchemaChoiceIndex {
  struct Impl;
  std::shared_ptr<const Impl> impl;
public:
  ProteinSchemaChoiceIndex(const ProteinSchemaDocument &, Id entry,
                           ProteinSchemaValueOptions = {});
  std::span<const ProteinSchemaChoiceEntry> entries() const noexcept;
  bool complete() const noexcept;
  ProteinSchemaChoiceLookup find_id(std::string_view) const;
  // XML factory value projection: scalar uses the first token; arrays keep
  // matched tokens in order, including repeats, and skip later missing IDs.
  // A missing first ID is not replaced with a fabricated default. Raw tokens
  // and matched entry identities remain available even when the result is
  // unresolved. Does not validate declaration flags/IDs or register properties.
  ProteinSchemaChoiceDefaults defaults(ProteinSchemaValueOptions = {}) const;
};
enum class ProteinSchemaAliasStatus {
  accepted, empty_name, empty_target, duplicate_name, duplicate_local_target
};
struct ProteinSchemaAliasBinding {
  const ProteinSchemaDocument *document = nullptr; // borrowed from owning index
  Id source = none; // XML PropertyAlias node in document
  std::string_view target; // exact property ID; no recursive alias expansion
};
struct ProteinSchemaAliasEntry {
  Id source = none;
  std::optional<std::string_view> name, target;
  ProteinSchemaAliasStatus status = ProteinSchemaAliasStatus::empty_name;
  std::optional<ProteinSchemaAliasBinding> conflict;
};
struct ProteinSchemaAliasOptions {
  uint32_t max_aliases = 1000000, max_visible_aliases = 1000000;
  uint32_t max_scope_depth = 256; // bounds retained parent/history ownership
  uint32_t max_parent_changes = 256; // constructor event array limit
};
class ProteinSchemaAliasIndex;
struct ProteinSchemaAliasParentChange {
  Id before_entry = none; // immediate XML child; events in strict source order
  const ProteinSchemaAliasIndex *parent = nullptr; // nullptr detaches the parent
};
// Immutable PropertyAlias registration and scoped lookup, sharing documents.
// Names must be unique across currently visible parent aliases; targets need
// only be unique locally and need not name an existing property. Retains all
// local attempts in source order. Alias lookup returns exactly one target;
// property lookup must check actual properties first, then that direct target.
// Optional parent_changes take effect before specified immediate XML entries,
// allowing Base/file loading to change visibility during declaration input.
// Does not accept properties, choose a parent, merge values or register schemas.
// Copies the visible parent table at each binding; lookup is O(log visible).
// Returned document pointers/views live as long as the index (or its copies).
class ProteinSchemaAliasIndex {
  struct Impl;
  std::shared_ptr<const Impl> impl;
  explicit ProteinSchemaAliasIndex(std::shared_ptr<const Impl>);
  std::optional<ProteinSchemaAliasBinding> find_before(
      Id source, std::string_view name) const;
  friend class ProteinSchemaDefinitionIndex;
public:
  explicit ProteinSchemaAliasIndex(const ProteinSchemaDocument &,
      const ProteinSchemaAliasIndex *visible_parent = nullptr,
      ProteinSchemaAliasOptions = {},
      std::span<const ProteinSchemaAliasParentChange> parent_changes = {});
  const ProteinSchemaDocument &document() const noexcept;
  std::span<const ProteinSchemaAliasEntry> entries() const noexcept;
  std::optional<ProteinSchemaAliasBinding> find(std::string_view name) const;
  // Bind/replace a parent after local registration. Existing accepted/rejected
  // attempts keep their outcomes and provenance; they are not re-registered.
  // Local accepted names shadow the newly visible parent. This preserves the
  // distinction between a parent visible during XML input and a deferred one.
  ProteinSchemaAliasIndex with_parent_aliases(const ProteinSchemaAliasIndex &,
      ProteinSchemaAliasOptions = {}) const;
};

enum class ProteinSchemaDefinitionStatus { ready, unsupported, unresolved };
enum class ProteinSchemaDeclarationStatus {
  property, metadata, alias, rejected, unsupported, unresolved, unprocessed
};
enum class ProteinSchemaDeclarationGate {
  not_evaluated, accepted, empty_id, existing_property, flag_conflicts
};
enum class ProteinSchemaPropertyContainer : uint32_t {
  ordinary = 0, iterator_data = 1, readonly_data = 2
};
struct ProteinSchemaDefinitionChoice {
  ProteinSchemaChoiceIndex index;
  ProteinSchemaChoiceDefaults defaults;
};
using ProteinSchemaDefinitionValue = std::variant<
    bool, ProteinSchemaTextDefaults, ProteinSchemaNumericDefaults,
    ProteinSchemaConstructedDefaults, ProteinSchemaDefinitionChoice>;
struct ProteinSchemaRuntimeBinding {
  std::string_view type_name, data_type; // static registered names; no DLL needed
};
// Identifies the concrete factory layout of a cached projection, independently
// of later inherited flags. Does not assert acceptance, a resolved value, unit
// conversion or link resolution. Unknown/unsupported layouts return nullopt.
std::optional<ProteinSchemaRuntimeBinding> protein_schema_runtime_binding(
    const ProteinSchemaDefinitionValue &) noexcept;
enum class ProteinSchemaPropertyOrigin { xml_declaration, load_default, load_rewrite };
struct ProteinSchemaDefinitionProperty {
  const ProteinSchemaDocument *document = nullptr;
  Id source = none; // XML context; none for a generated load default
  ProteinSchemaPropertyOrigin origin = ProteinSchemaPropertyOrigin::xml_declaration;
  std::string_view id, label;
  uint32_t value_type = 0, flags = 0;
  ProteinSchemaPropertyContainer container = ProteinSchemaPropertyContainer::ordinary;
  bool active = true;
  Id previous = none, replaced = none; // history indexes in this snapshot
  const ProteinSchemaDocument *flags_document = nullptr;
  Id flags_source = none; // most recent flags supplier; value origin stays above
  std::shared_ptr<const ProteinSchemaDefinitionValue> value;
};
enum class ProteinSchemaComparisonStatus { equal, different, unresolved };
enum class ProteinSchemaComparisonIssue {
  none, missing_value, unknown_layout, unavailable_projection, unknown_number,
  unknown_unit, unassigned_type_default, incomplete_choice,
  short_choice_table, inconsistent_shape, unknown_constraint,
  null_left_constraint, raw_layout_required, unsafe_raw_access
};
struct ProteinSchemaComparisonResult {
  ProteinSchemaComparisonStatus status = ProteinSchemaComparisonStatus::unresolved;
  ProteinSchemaComparisonIssue issue = ProteinSchemaComparisonIssue::none;
  uint64_t ordinal = 0; // value element or accepted Choice-table ordinal
};
struct ProteinSchemaComparisonOptions {
  uint64_t max_values = 1000000, max_text_bytes = 64ull << 20;
};
// Native compatibility predicates over immutable cached factory projections.
// equal is a native zero/true result, not a deduplication or identity relation:
// scalar NaN can compare equal and Reference/Texture compare concrete classes.
// Choice table comparison is directional. Unsafe/unknown inputs are explicit;
// budget exhaustion throws Error. No parsing, IO or product DLL execution.
ProteinSchemaComparisonResult protein_schema_compare_values(
    const ProteinSchemaDefinitionValue &, const ProteinSchemaDefinitionValue &,
    ProteinSchemaComparisonOptions = {});
// Directional comparison of XML-created Choice tables (empty native header
// string), before selected values. Known rejected entries are skipped; uncertain
// prefixes and a short right table remain explicit unresolved states.
ProteinSchemaComparisonResult protein_schema_compare_choice_tables(
    const ProteinSchemaChoiceIndex &, const ProteinSchemaChoiceIndex &,
    ProteinSchemaComparisonOptions = {});
// A null property differs even from null. Checks type, then skip_type (which
// bypasses all remaining checks), type6/7 bounds or type10 Choice table, then
// the concrete values. Does not compare flags, labels, IDs or aliases, perform
// version/URI upgrades, copy anything or register definitions.
ProteinSchemaComparisonResult protein_schema_compare_properties(
    const ProteinSchemaDefinitionProperty *, const ProteinSchemaDefinitionProperty *,
    uint32_t skip_type = 0, ProteinSchemaComparisonOptions = {});
struct ProteinSchemaDefinitionDeclaration {
  Id source = none;
  std::string_view effective_id;
  ProteinSchemaDeclarationStatus status = ProteinSchemaDeclarationStatus::unprocessed;
  ProteinSchemaDeclarationGate gate = ProteinSchemaDeclarationGate::not_evaluated;
  uint32_t flag_conflicts = 0;
  Id property = none, gate_conflict = none, alias = none;
  std::optional<ProteinSchemaIdentityAssignment> identity;
  // Retained factory projection, including rejected/unresolved candidates.
  std::shared_ptr<const ProteinSchemaDefinitionValue> candidate;
};
enum class ProteinSchemaDefinitionLookupStatus { found, missing, unresolved };
struct ProteinSchemaDefinitionLookup {
  ProteinSchemaDefinitionLookupStatus status = ProteinSchemaDefinitionLookupStatus::missing;
  Id property = none; // properties() history index, only when found
};
struct ProteinSchemaDefinitionOptions {
  uint32_t max_declarations = 1000000, max_property_history = 1000000;
  uint64_t max_cached_values = 1000000; // aggregate retained local values/choice rows
  uint32_t max_inheritance_depth = 256;
  ProteinSchemaValueOptions values;
  ProteinSchemaAliasOptions aliases;
};
// Algorithm profiles for observed native property iteration, not file-year
// detection. The older profile orders name bytes as signed char; the newer
// profile uses unsigned bytes. Equal names retain the current merge group.
enum class ProteinSchemaIterationProfile { reader_2017, reader_2026 };
struct ProteinSchemaIterationOptions {
  ProteinSchemaIterationProfile profile = ProteinSchemaIterationProfile::reader_2026;
  bool public_only = false, reverse = false;
};
// Immutable local XML acceptance and explicit parent-property merge. Reuses
// the value/identity/alias readers; owns document views and shares cached values.
// Actual property lookup scans ordinary/iterator/readonly containers, then
// expands one alias target. Replacements/moves retain inactive history rows.
// ready describes known declaration acceptance, not resolved numeric text or
// downstream DateTime/UUID/URL formats. Candidate projections retain their own
// conversion states. An uncertain acceptance stops inference; later XML stays
// available as unprocessed rows. Missing queries then return unresolved.
// Unknown factories are explicit unsupported rows and do not silently become
// complete definitions. Known diagnostic rejection paths allow later rows.
// No external IO, Base-name dependency solving, postload SchemaVersion or global
// registration is implied. Parent alias visibility during XML input is explicit;
// parent property copying happens only through with_inherited_parent().
class ProteinSchemaDefinitionIndex {
  struct Impl;
  std::shared_ptr<const Impl> impl;
  explicit ProteinSchemaDefinitionIndex(std::shared_ptr<const Impl>);
  friend class ProteinSchemaDefinitionLoadIndex;
public:
  explicit ProteinSchemaDefinitionIndex(const ProteinSchemaDocument &,
      const ProteinSchemaAliasIndex *visible_alias_parent = nullptr,
      ProteinSchemaDefinitionOptions = {},
      std::span<const ProteinSchemaAliasParentChange> parent_changes = {});
  const ProteinSchemaDocument &document() const noexcept;
  ProteinSchemaDefinitionStatus status() const noexcept;
  std::span<const ProteinSchemaDefinitionDeclaration> declarations() const noexcept;
  std::span<const ProteinSchemaAliasEntry> aliases() const noexcept;
  std::optional<ProteinSchemaAliasBinding> find_alias(std::string_view) const;
  std::string_view uid() const noexcept;
  Id uid_source() const noexcept; // none if no successful UID assignment
  std::span<const ProteinSchemaDefinitionProperty> properties() const noexcept;
  // Active indexes in lexicographic ID order; not a claim of native iteration order.
  std::span<const Id> container(ProteinSchemaPropertyContainer) const;
  // Active actual-property history indexes in native merge order. Preserves
  // cross-container duplicates, excludes aliases and filters public flags only
  // after advancing the unfiltered merge. Reverse iteration has its own tie
  // order and is not simply a reversed forward result. No values are copied.
  // For a non-ready definition, this is its accepted prefix; consult status()
  // before treating the sequence as complete. The index owns referenced data.
  std::vector<Id> iteration(ProteinSchemaIterationOptions = {}) const;
  ProteinSchemaDefinitionLookup find(std::string_view) const;
  // Requires known acceptance in both indexes. Shares values/origins; retains
  // child values and merges flags as parent | (child & 0x180), even if flags now
  // describe a different value shape. Does not repeat local alias registration.
  ProteinSchemaDefinitionIndex with_inherited_parent(
      const ProteinSchemaDefinitionIndex &, ProteinSchemaDefinitionOptions = {}) const;
};

enum class ProteinSchemaPropertyField {
  identity, data, flags, label, constraint, choice, control
};
struct ProteinSchemaPropertyFieldSource {
  const ProteinSchemaDefinitionIndex *definition = nullptr;
  Id property = none;
};
enum class ProteinSchemaReferencePresence { absent, present, unresolved };
enum class ProteinSchemaPropertyCopyIssue {
  none, missing_value, unknown_layout, raw_layout_required, unknown_constraint
};
struct ProteinSchemaPropertyCopyResult;

// Immutable property state with separate target identity and data/metadata
// suppliers. Owns the indexed rows and their document views. Copying shares
// immutable payloads and flattens field ownership; it retains no state chains.
// Target class/ID/context stay fixed. Source flags/label replace the target;
// source bounds/Choice replace only when present, otherwise target refs remain.
// Array data carries the source's actual element type, even across array classes.
// Arbitrary cross-POD byte reinterpretation stays unresolved, never converted or
// reported as native rejection. This is a logical state operation, not product
// allocation/exception emulation, namespace registration or an index mutation.
class ProteinSchemaPropertyState {
  struct Impl;
  std::shared_ptr<const Impl> impl;
  explicit ProteinSchemaPropertyState(std::shared_ptr<const Impl>);
public:
  explicit ProteinSchemaPropertyState(const ProteinSchemaDefinitionIndex &, Id property);
  // Initial target row, including its original flags/container/label. Use the
  // state accessors for current data/metadata. Includes inactive history rows.
  const ProteinSchemaDefinitionProperty &target() const noexcept;
  std::optional<ProteinSchemaRuntimeBinding> binding() const noexcept;
  // Supplier's factory projection, including its original class/default markers
  // and diagnostics. Its metadata is not the current state metadata. Consult
  // data_binding() for actual data, binding() for the fixed target class.
  const std::shared_ptr<const ProteinSchemaDefinitionValue> &data() const noexcept;
  std::optional<ProteinSchemaRuntimeBinding> data_binding() const noexcept;
  // Opaque PropertyBase runtime control byte; all 21 known factory classes
  // initialize it to 1, including lists. It is not the array flag. Its full
  // semantic name is unresolved; this is not a file offset or a data conversion.
  std::optional<uint8_t> control_byte() const noexcept;
  // Mirrors the observed raw-byte setter without changing class, data or
  // metadata. Requires a known control layout; throws Error otherwise. Explicit
  // mutations have no indexed-row supplier. 2..255 retain raw instruction
  // inputs, not a claim that they are valid native C++ bool arguments.
  ProteinSchemaPropertyState with_control_byte(uint8_t) const;
  uint32_t flags() const noexcept;
  std::string_view label() const noexcept;
  ProteinSchemaReferencePresence constraint_presence() const noexcept;
  // Candidate is also available when presence is unresolved; null when absent.
  const ProteinSchemaResolvedConstraint *constraint() const noexcept;
  const ProteinSchemaChoiceIndex *choice_table() const noexcept;
  // A supplier is an indexed row in this retained scope, not a global NWD ID.
  // Absent references have no supplier. Flags rows can themselves carry a
  // different flags_document/flags_source after inheritance.
  ProteinSchemaPropertyFieldSource supplier(ProteinSchemaPropertyField) const;
  ProteinSchemaPropertyCopyResult copied_from(const ProteinSchemaPropertyState &) const;
};
struct ProteinSchemaPropertyCopyResult {
  ProteinSchemaPropertyCopyIssue issue = ProteinSchemaPropertyCopyIssue::none;
  // Present only when all field rules can be applied. Unresolved conversion
  // diagnostics within a shared payload remain intact. No input is changed.
  std::optional<ProteinSchemaPropertyState> state;
};

// Native property compatibility over current state fields. Checks target type
// and skip_type, current bounds/Choice, raw control byte, fixed concrete class,
// then actual data. The control gate also applies to Reference/Texture.
// Arrays dispatch the left actual element comparator and use the right actual
// stride. Stable int32/double/color4 layouts are read lazily as little endian
// POD bytes; unsafe reads and mixed string ABI layouts stay unresolved. Does
// not compare flags/labels/IDs, register anything or treat equality as identity.
ProteinSchemaComparisonResult protein_schema_compare_property_states(
    const ProteinSchemaPropertyState *, const ProteinSchemaPropertyState *,
    uint32_t skip_type = 0, ProteinSchemaComparisonOptions = {});

enum class ProteinSchemaDefinitionLoadStatus {
  dependencies_resolved, pending_dependencies, unresolved
};
enum class ProteinSchemaDefinitionLoadRecordStatus {
  resolved, pending, duplicate, rejected, unresolved, unprocessed
};
enum class ProteinSchemaDefinitionLoadIssue {
  none, empty_base, uncertain_local_definition, incompatible_base,
  incompatible_version, uncertain_version, missing_common_text,
  incompatible_common_text
};
struct ProteinSchemaDefinitionLoadRecord {
  std::string_view uid, base;
  ProteinSchemaDefinitionLoadRecordStatus status = ProteinSchemaDefinitionLoadRecordStatus::unprocessed;
  ProteinSchemaDefinitionLoadIssue issue = ProteinSchemaDefinitionLoadIssue::none;
  Id canonical = none, parent = none, waiting_on = none, cycle = none;
  Id issue_property = none; // property history index in definition(), if known
  bool duplicate_was_pending = false, version_dimension_invalid = false;
  std::optional<int32_t> common_version; // ordinal0 read only by the root branch
};
enum class ProteinSchemaDefinitionLoadEventKind {
  default_property, rewrite_property, version_invalid_dimension,
  queued, resolved, duplicate, rejected, unresolved
};
struct ProteinSchemaDefinitionLoadEvent {
  ProteinSchemaDefinitionLoadEventKind kind = ProteinSchemaDefinitionLoadEventKind::unresolved;
  Id record = none, property = none, previous = none, related = none;
  std::string_view name; // requested name, possibly an alias of actual property
};
struct ProteinSchemaDefinitionLoadCycle {
  std::vector<Id> records; // cycle members in Base-following order; no closing repeat
};
enum class ProteinSchemaCommonRequirementStatus { matched, missing, flags_mismatch };
struct ProteinSchemaCommonRequirement {
  Id record = none, property = none;
  std::string_view name;
  uint32_t required_flags = 0; // compare actual flags & 0xe; not a type check
  ProteinSchemaCommonRequirementStatus status = ProteinSchemaCommonRequirementStatus::missing;
};
enum class ProteinSchemaDefinitionLoadLookupStatus { found, pending, missing, unresolved };
struct ProteinSchemaDefinitionLoadLookup {
  ProteinSchemaDefinitionLoadLookupStatus status = ProteinSchemaDefinitionLoadLookupStatus::missing;
  Id record = none; // canonical input index, only for found/pending
};
struct ProteinSchemaDefinitionLoadOptions {
  uint32_t max_sources = 100000, max_events = 1000000;
  uint64_t max_total_declarations = 1000000, max_total_property_history = 2000000;
  uint64_t max_total_cached_values = 1000000;
  ProteinSchemaDefinitionOptions definition;
};
// One explicit batch namespace. Inputs are cached local XML definition indexes,
// before explicit inheritance or a previous load's generated/rewritten values.
// Copies share their values/documents; input order controls admission. UID keys
// are exact strings. Both ready and pending duplicates retain the first identity;
// definition(duplicate) follows that canonical object's final snapshot.
// Applies post-XML SchemaVersion/CommonSchema values before named inheritance.
// Pending promotions preserve the native smallest-ready-UID order using reverse
// dependencies, without repeated whole-table scans. Cycles are derived diagnostics.
// Known empty-Base rejection continues admission. Uncertain semantics retain the
// processed prefix and stop later admission; input() still exposes every input.
// dependencies_resolved can coexist with rejected rows; inspect record statuses.
// Dependency readiness and the 17 direct-name CommonSchema flag checks do not
// assert global registration/compatibility. No file IO, cross-batch UID merging,
// automatic model/library scope construction or product callbacks are performed.
// Aggregate limits charge logical input rows and distinct output snapshots;
// inherited values are shared rather than charged as new cache allocations.
// All returned views/pointers live with this immutable index or its copies.
class ProteinSchemaDefinitionLoadIndex {
  struct Impl;
  std::shared_ptr<const Impl> impl;
public:
  explicit ProteinSchemaDefinitionLoadIndex(
      std::span<const ProteinSchemaDefinitionIndex>, ProteinSchemaDefinitionLoadOptions = {});
  ProteinSchemaDefinitionLoadStatus status() const noexcept;
  std::span<const ProteinSchemaDefinitionLoadRecord> records() const noexcept;
  const ProteinSchemaDefinitionIndex &input(Id record) const;
  // nullptr means this input was not admitted. Rejected/unresolved inputs may
  // expose a partial snapshot. Check records()[record].status before use.
  const ProteinSchemaDefinitionIndex *definition(Id record) const;
  ProteinSchemaDefinitionLoadLookup find_uid(std::string_view) const;
  std::span<const ProteinSchemaDefinitionLoadEvent> events() const noexcept;
  std::span<const Id> pending() const noexcept; // ordered by UID, not source ordinal
  std::span<const ProteinSchemaDefinitionLoadCycle> cycles() const noexcept;
  // Empty unless the canonical CommonSchema reached the ready namespace.
  std::span<const ProteinSchemaCommonRequirement> common_requirements() const noexcept;
};
// Index one immutable graph once. It must outlive the index and returned views.
// Connection slots and URI ordinals retain their original property scope.
// No shader-role inference or runtime conversion to JSON is performed.
class ProteinGraphIndex {
  struct Impl;
  std::shared_ptr<const Impl> impl;

public:
  explicit ProteinGraphIndex(const ObjectGraph &, Id graph_id = 0);
  const ObjectGraph &graph() const;
  std::span<const ProteinConnection> connections(Id owner) const;
  std::span<const ProteinUriSlot> uris(Id owner) const;
  const ProteinUriSlot &uri(Id owner, Id property, Id ordinal) const;
  std::vector<ProteinReachableAsset> reachable(Id root) const;
};
struct Appearance {
  uint32_t flags = 0;
  Id material = none, asset = none;
  std::array<uint32_t, 4> overrides{};
  std::optional<uint32_t> legacy_discarded_enum; // pre405 bit2 without bit8
  bool legacy_face_mode_default =
      false; // pre405 bit8 without bit2: native 0x1b01
};
struct LegacyAppearanceObject {
  Id owner = none;
  uint64_t excluded_fields = UINT64_MAX; // zero bits select serialized fields
  std::array<Id, 2> materials{none, none};
  std::optional<uint32_t> saved_boolean, saved_enum;
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
enum class GeometryShapePath { object, ordinal, link };
struct GeometryShapeObject {
  Id owner = none, path_object = none, transform = none, appearance = none,
     geometry_reference = none, auxiliary_transform = none;
  GeometryShapePath path_encoding = GeometryShapePath::object;
  uint32_t path_reference = UINT32_MAX; // raw ordinal/link, not a resolved path
  uint32_t primitive_count = 0, bits = 0, flags = 0;
  double precision = 0;
};
struct GeometryAuxiliaryObject {
  Id owner = none; // shape owner; table index is distinct from graph object ID
  AuxiliaryTransform fields;
};
enum class InlinePathStatus { empty, no_exact_match, unique, ambiguous };
struct InlinePathBinding {
  Id owner = none; // Path44/RunPath73 object in partition graph0
  InlinePathStatus status = InlinePathStatus::empty;
  uint32_t offset = 0, count = 0; // range in path_candidates
};
enum class InlineShapePathStatus {
  null_path,
  empty_path,
  different_root,
  non_geometry_terminal,
  no_link,
  context_required,
  links_not_ready,
  resolved
};
struct InlineShapeBinding {
  Id owner = none, path_binding = none; // null saved path stays none
  Id path = none; // resolved Model.paths ID, never a spatial occurrence ID
  InlineShapePathStatus status = InlineShapePathStatus::null_path;
};
struct InlineSpatialRoot {
  Id partition = none, occurrence = none; // null spatial root stays none
};
struct InlinePartitionIndex {
  // Candidate IDs index Model.paths. Equal node sequences may describe more
  // than one occurrence; no arbitrary winner is selected. Ranges are shared.
  // Exact complete root-to-node sequences only, not native FindLink's runtime
  // search/fallback policy. No-exact-match does not establish a corrupt path.
  std::vector<Id> path_candidates;
  std::vector<InlinePathBinding> path_bindings;
  // Read-time shape binding has a root/geometry-terminal gate. For a valid
  // full path, native link search selects its first preorder occurrence;
  // candidates remain available even when more than one occurrence matches.
  std::vector<InlineShapeBinding> shape_bindings;
  // Separate occurrence domain, preserving spatial child slots and DAG reuse.
  // Path.object indexes graph0; Path.parent indexes spatial_occurrences.
  std::vector<Path> spatial_occurrences;
  std::vector<InlineSpatialRoot> spatial_roots;
};
struct InlineObjectBinding {
  Id owner = none, value = none; // graph0 identity -> corresponding Model table
};
struct InlineModelProjection {
  std::vector<InlineObjectBinding> geometries, geometry_references, transforms,
      materials, appearances, assets, instances;
  // These old appearance fields remain in graph0's geometry arena rather
  // than being guessed into modern appearance flags/overrides.
  std::vector<Id> legacy_appearance_fields;
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
      instances; // modern fragment slots (including repeats); old inline models
                 // project each bound shape object once; see inline_projection
  std::vector<ObjectGraph>
      graphs; // 0 partition, 1 hierarchy, 2+ property pages
  std::vector<Path> paths;
  Id hierarchy_graph = 1; // inline old logical nodes share partition graph0
  std::shared_ptr<const InlinePartitionIndex> inline_partition;
  std::shared_ptr<const InlineModelProjection> inline_projection;
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
  uint32_t projection = 0; // 0=perspective, 1=orthographic
  std::array<double, 3> position{};
  std::array<double, 4> orientation{}; // XYZW
  // Aspect, height/angle, near, far, then version >=425 up/right offset
  // factors.
  std::array<double, 6> parameters{};
  bool offset_factors_present = false;
  double aspect_ratio() const noexcept { return parameters[0]; }
  // Full vertical angle in radians for perspective; full height for
  // orthographic.
  double height_field() const noexcept { return parameters[1]; }
  double near_distance() const noexcept { return parameters[2]; }
  double far_distance() const noexcept { return parameters[3]; }
  // Fractions of the vertical/horizontal extent, respectively. Absence is
  // different from a stored zero. Raw parameters remain available unchanged.
  std::optional<double> up_offset_factor() const noexcept {
    return offset_factors_present ? std::optional<double>(parameters[4])
                                  : std::nullopt;
  }
  std::optional<double> right_offset_factor() const noexcept {
    return offset_factors_present ? std::optional<double>(parameters[5])
                                  : std::nullopt;
  }
};
// Returns a transformed copy using a caller-supplied column-major affine matrix
// in the camera's coordinate units. The stored XYZW components are used as-is;
// the composed pose is decomposed into a proper rotation. Orthographic height,
// near and far distances use the mean absolute principal scale; perspective
// parameters, aspect and offset-factor presence/values are retained. Throws
// Error for nonfinite matrix/pose components, non-affine matrices, singular or
// numerically unreliable poses, or nonfinite transformed distances. Does not
// infer NWF/world placement.
Camera transformed_camera(const Camera &, const std::array<double, 16> &);
// Secondary fields retain their serialization order; see docs/FORMAT.md.
struct ViewFields {
  std::vector<uint32_t> integers;
  std::vector<double> numbers;
  std::vector<std::string> strings;
};
struct ViewpointRenderSettings {
  // Saved viewpoint settings, separate from the Camera near/far parameters.
  double near_distance = 0, far_distance = 0;
  // Distance types: 0=fixed, 1=constrained, 2=automatic.
  uint32_t near_distance_type = 0, far_distance_type = 0;
  // 0=mask, 1=fill, 2=preserve vertical, 3=preserve horizontal, 4=overscan.
  uint32_t image_fit = 0;
  double horizontal_scale = 0, aperture_diameter = 0, shutter_speed = 0;
};
struct ViewpointState {
  std::optional<std::array<double, 3>> world_up;
  std::optional<double> focal_distance, linear_speed, angular_speed;
  // tool preserves modern identifiers and converts pre-120 identifiers.
  std::optional<uint32_t> serialized_tool, tool;
  std::optional<std::array<double, 2>> tilt_limits;
  std::optional<uint32_t> lighting;
  // 0=full, 1=preview, 2=shaded, 3=wireframe, 4=hidden line.
  std::optional<uint32_t> render_style;
  std::optional<uint32_t> primitives;
  std::optional<double> preferred_fov;
  // Always serialized from version425, independently of the parts mask.
  std::optional<ViewpointRenderSettings> render_settings;
};
struct ViewerState {
  double radius = 0, height = 0, actual_height = 0, eye_height_offset = 0;
  // Borrowed from CurrentView::viewer; invalidated by mutation/destruction.
  // nullopt is a stored NULL; an empty string_view is a stored empty string.
  std::optional<std::string_view> avatar;
  uint32_t camera_mode =
      0; // 0=first person, 1=third person; retains unknown codes
  double first_to_third_angle = 0, first_to_third_distance = 0;
  double first_to_third_param = 0; // multiplier of first_to_third_distance
  bool first_to_third_correction = false, collision_detection = false,
       gravity = false;
  // Stored from version50 and version55, respectively; never invented defaults.
  std::optional<double> gravity_value, terminal_velocity;
  std::optional<bool> auto_crouch;
  // Applies an explicit signed scale to dimensions, third-person distance,
  // gravity value and terminal velocity. Angles and the multiplier stay intact.
  // Preserves missing fields; does not infer a project transform or run
  // physics.
  ViewerState scaled(double factor) const noexcept;
};
struct LegacyClipPlane {
  bool enabled = false;
  double distance = 0;
  std::array<double, 3> normal{};
  double legacy_value = 0; // retained field discarded by investigated loaders
};
struct ClipPlaneFrame {
  uint32_t state =
      0; // 0=default, 1=enabled, 2=disabled; preserves unknown codes
  std::array<double, 3> location{}, direction{}, x_direction{};
};
struct ClipPlane {
  uint32_t alignment = 0; // raw alignment identifier
  // Version116 replaces the legacy plane equation with a surface frame.
  // Vectors are saved values, without loader normalization or
  // orthogonalization.
  std::variant<LegacyClipPlane, ClipPlaneFrame> value;
};
struct ClipBounds {
  std::array<double, 3> min{}, max{};
};
struct ClipSettings {
  bool linked = false;
  int32_t current_plane = 0;
  ClipBounds range; // saved range, separate from the clipping box
  // Stored from version107, independent of the selected mode.
  std::optional<uint32_t> mode; // 0=planes, 1=box; preserves unknown codes
  std::optional<bool> enabled;
  std::optional<ClipBounds> box;
  // Stored from version118. XYZW, not normalized or replaced with identity.
  std::optional<std::array<double, 4>> box_orientation;
};
// Interpret saved clip arrays, including those on AnimationKeyFrame.
// No allocation or normalization; inconsistent arrays throw Error.
ClipPlane clip_plane_fields(const ViewFields &, uint32_t wire_version);
ClipSettings clip_settings_fields(const ViewFields &, uint32_t wire_version);
// Loaded geometry, separate from the serialized ClipPlaneFrame and optional
// ClipSettings. All axes are retained, including degenerate/left-handed frames.
struct SurfaceFrame {
  std::array<double, 3> location{}, x{}, y{}, z{};
};
struct ClipBoxState {
  ClipBounds box, range;
  std::array<double, 4> orientation{0, 0, 0, 1};
};
// Explicit implementation generation, not inferred from the saved wire version.
enum class ClipLoadProfile { reader_2017, reader_2026 };
struct LoadedClipPlane {
  uint32_t state = 0, alignment = 0;
  SurfaceFrame frame;
};
struct LoadedClipSet {
  std::array<LoadedClipPlane, 6> planes;
  bool linked = false, enabled = false;
  int32_t current_plane = 0;
  uint32_t mode = 0;
  ClipBoxState bounds;
  std::array<double, 16> box_transform{1, 0, 0, 0, 0, 1, 0, 0,
                                       0, 0, 1, 0, 0, 0, 0, 1};
  // Provenance of loading repairs/defaults, retained through transformations.
  ClipLoadProfile profile = ClipLoadProfile::reader_2017;
  uint32_t wire_version = 0;
  bool box_reset = false, orientation_defaulted = false,
       orientation_nan_reset = false;
  bool box_defaulted = false;
  uint32_t serialized_plane_count = 6;
};
struct ClipPlaneEquation {
  std::array<double, 3> normal{};
  double distance = 0; // dot(normal, position) = distance
  uint32_t slot = 0; // source plane index or box-face index, never compacted ID
};
struct ActiveClipPlanes {
  std::array<ClipPlaneEquation, 6> planes;
  uint32_t count = 0;
};
// Fresh load with an explicit implementation profile (not a file-year guess).
// Wire versions >=18; present records require their original declared count.
// A completely absent set returns nullopt; inconsistent/unsupported fields,
// unknown profiles and invalid numerical operations throw Error. The input is
// unchanged, including nonfinite saved fields repaired by the selected profile.
// Fixed-size result and scratch storage; no allocation on the successful path.
std::optional<LoadedClipSet>
loaded_clip_set(std::span<const ViewFields> planes, const ViewFields &settings,
                std::optional<uint32_t> declared_count, uint32_t wire_version,
                ClipLoadProfile profile);
// Reads the loaded snapshot. Disabled/unknown-mode sets and empty boxes yield
// count0. Plane mode includes exactly state1; its normals can be nonunit/zero.
// Box faces use the cached column-major rotation-tagged box_transform. Preserve
// it with the bounds/orientation; editing those fields does not refresh it.
// Only entries [0,count) are valid; slot preserves the source index (0..5).
// Equations are dot(normal,position)=distance, with the retained half-space >=.
// Uses source coordinates, with no automatic NWF/world placement or rendering.
ActiveClipPlanes active_clip_planes(const LoadedClipSet &);
struct CurrentView {
  std::string chunk_name;
  uint32_t parts = 0;
  Camera camera;
  ViewFields viewer, state, clip_set;
  std::vector<ViewFields> clip_planes;
  uint32_t wire_version = 0;
  bool viewer_avatar_is_null = false;
  // Legacy count0 still follows one independently serialized first plane.
  std::optional<uint32_t> clip_declared_plane_count;
  // Interprets the current raw arrays without allocation or cached copies.
  // Absent fields remain absent. Invalid or inconsistent arrays throw Error.
  ViewpointState named_state() const;
  // nullopt means parts2 is absent; avatar strings borrow storage from this.
  std::optional<ViewerState> named_viewer() const;
  ClipPlane named_clip_plane(size_t index) const;
  // nullopt for viewpoint-only records that do not store a clip set.
  std::optional<ClipSettings> named_clip_settings() const;
  std::optional<LoadedClipSet> loaded_clipping(ClipLoadProfile) const;
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
enum class SearchOperator : uint32_t {
  never = 0,
  has_category = 1,
  no_category = 2,
  has_property = 3,
  no_property = 4,
  same_type = 5,
  equals = 6,
  not_equals = 7,
  less_than = 8,
  less_equal = 9,
  greater_equal = 10,
  greater_than = 11,
  contains = 12,
  wildcard = 13,
  within_day = 14,
  within_week = 15
};
enum class SearchOption : uint32_t {
  category_internal_name = 1,
  category_display_name = 2,
  property_internal_name = 4,
  property_display_name = 8,
  ignore_value_case = 16,
  negate = 32,
  start_group = 64,
  ignore_value_accents = 128,
  ignore_value_character_widths = 256
};
struct SearchCondition {
  Id category = none, property = none; // SavedItems.objects name objects
  uint32_t condition = 0, options = 0; // stream order: options, condition
  Value value; // strings and graph=0 objects belong to SavedItems.objects
  std::optional<SearchOperator> operation() const {
    if (condition <= static_cast<uint32_t>(SearchOperator::within_week))
      return static_cast<SearchOperator>(condition);
    return {};
  }
  bool has_option(SearchOption option) const {
    return (options & static_cast<uint32_t>(option)) != 0;
  }
  uint32_t unknown_options() const { return options & ~uint32_t{0x1ff}; }
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
enum class AnimationObjectKind : uint32_t {
  show_viewpoint = 150,
  set_variable = 151,
  pause = 152,
  play_animation = 153,
  stop_animation = 154,
  message = 155,
  load_model = 156,
  store_property = 157,
  on_key_press = 158,
  sphere = 159,
  selection_sphere = 160,
  on_hotspot = 161,
  on_variable = 162,
  on_script = 163,
  on_timer = 164,
  on_animation = 165,
  on_collision = 166,
  on_start = 167
};
enum class AnimationEnum : uint32_t {
  play_start,
  play_end,
  stop_animation,
  variable_modifier,
  variable_comparison,
  timer,
  script_trigger,
  animation_trigger,
  key_trigger,
  hotspot_trigger,
  hotspot_type
};
// Stable, nonlocalized names; unknown domain/value returns an empty view.
// Raw values remain in the records. Play start/end have different value maps.
std::string_view animation_enum_name(AnimationEnum, int32_t) noexcept;
struct AnimationPathRecord {
  std::pair<std::string, std::string> item_path;
  std::optional<int32_t> mode; // absent for show_viewpoint
};
struct AnimationVariableRecord {
  std::string name;
  Value value;           // owning ObjectGraph's strings and references
  int32_t operation = 0; // source assignment/comparison enumeration
};
struct AnimationTimeRecord {
  double delay = 0;            // seconds
  std::optional<int32_t> mode; // absent for pause
};
struct AnimationPlayRecord {
  std::pair<std::string, std::string> animation_path;
  bool finish = false; // pause at end (native option name "finish")
  int32_t start_type = 0, end_type = 0;
  double start_time = 0, end_time = 0; // seconds; interpreted by start/end type
};
struct AnimationTextRecord {
  std::string text;
};
struct AnimationPropertyRecord {
  SavedSelection selection;
  std::string variable, category, property;
};
struct AnimationKeyRecord {
  uint16_t key = 0;
  int32_t mode = 0;
};
struct AnimationSphereRecord {
  std::array<double, 3> center{};
  double radius = 0;
};
struct AnimationSelectionSphereRecord {
  SavedSelection selection;
  double radius = 0;
};
struct AnimationHotspotRecord {
  Reference hotspot;
  int32_t mode = 0;
};
struct AnimationCollisionRecord {
  SavedSelection selection;
  bool gravity = false;
};
using AnimationObjectValue =
    std::variant<std::monostate, AnimationPathRecord, AnimationVariableRecord,
                 AnimationTimeRecord, AnimationPlayRecord, AnimationTextRecord,
                 AnimationPropertyRecord, AnimationKeyRecord,
                 AnimationSphereRecord, AnimationSelectionSphereRecord,
                 AnimationHotspotRecord, AnimationCollisionRecord>;
struct AnimationObject {
  Id owner = none; // ObjectGraph.objects ID; repeated references stay shared
  AnimationObjectKind kind = AnimationObjectKind::on_start;
  AnimationObjectValue value;
};
// Borrows the typed record. Non-animation objects return nullptr; an ID outside
// the graph (including none) throws Error. The graph must remain unchanged.
const AnimationObject *animation_object(const ObjectGraph &, Id);
struct ScriptCondition {
  bool negated = false, open_bracket = false, close_bracket = false;
  bool has_operator = false;
  uint8_t operator_code = 0; // 0=AND, 1=OR; other codes retained unchanged
  Id event = none;           // SavedItems.objects ID
};
struct SavedScript {
  bool enabled = false;
  std::vector<ScriptCondition> conditions;
  std::vector<Id> actions; // SavedItems.objects IDs, including null/repetition
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
  std::optional<uint32_t> clip_declared_plane_count;
  // Uses the containing document's wire version; absent clipping stays absent.
  std::optional<LoadedClipSet> loaded_clipping(uint32_t wire_version,
                                               ClipLoadProfile) const;
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
  // Saved item80: SavedItems.objects, type182 before wire443, type185 from443.
  Id material_asset = none;
  std::optional<SavedSheetInfo> sheet_info;
  std::optional<SavedFileInfo> file_info;
  std::optional<SavedLight> light;
  uint32_t type = 0;
  Id parent = none;
  uint64_t offset = 0, end_offset = 0; // decoded chunk offsets
  bool complete = false;
  bool name_is_null = false; // retains the stream's null-string marker
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
  std::optional<SavedScript> script;
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
  // load_flags: bit0 replaces source units; bit1 replaces the base transform;
  // bit2 replaces up/north/front vectors. Before version244 these are all set.
  // The orientation_flag below is a separate transform execution hint.
  uint32_t flags = 0, load_flags = 7, linear_units = 0, angular_units = 0,
           orientation_flag = 0, extra_enum = 0;
  std::array<double, 12> affine{}; // native row-major 3x3, then translation
  // Stored vectors, not the resolved loading orientation. Front is zero
  // before109.
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
// ElementItemPath separates names with LF, not slash. Empty components are
// significant; an entirely empty path denotes the element's implicit root.
std::vector<std::string> split_saved_item_path(std::string_view,
                                               uint64_t max_parts = 20000000);
enum class SavedItemPathStatus {
  missing,
  resolved,
  element_root,
  unavailable,
  ambiguous_element,
  context_required
};
enum class SavedItemPathSemantics {
  require_context,     // empty components differ between native generations
  modern,              // resolve empty components as null/empty child names
  legacy_stop_at_empty // an empty component returns the current group/item
};
struct SavedItemPathResult {
  SavedItemPathStatus status = SavedItemPathStatus::missing;
  Id block = none, item = none; // ProductData block and SavedItems item IDs
  // Native lookup picks the first sibling in source order. This flag records
  // duplicates encountered along that route, including on unsuccessful paths.
  bool duplicate_names = false;
};
struct ProductData;
// Immutable index for one ProductData/source. owner_chunk supplies the exact
// namespace; element_name is ElementItemPath.first and path is .second.
class SavedItemPathIndex {
  struct Impl;
  std::shared_ptr<const Impl> impl_;

public:
  explicit SavedItemPathIndex(const ProductData &,
                              uint64_t max_items = 20000000);
  SavedItemPathResult resolve(
      std::string_view owner_chunk, std::string_view element_name,
      std::string_view path,
      SavedItemPathSemantics = SavedItemPathSemantics::require_context) const;
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
enum class ExternalPathStatus { null_path, original, remapped, ambiguous };
struct ExternalPathResolution {
  ExternalPathStatus status = ExternalPathStatus::original;
  std::string_view path;
  std::span<const Id> entries; // saved table positions, including duplicates
};
enum class ExternalReadPathStatus {
  null_path,
  original,
  remapped,
  invalid_encoding
};
struct ExternalReadPathResolution {
  ExternalReadPathStatus status = ExternalReadPathStatus::original;
  std::u16string
      path;        // owned native UTF-16 units, including unpaired surrogates
  Id entry = none; // last matching saved table entry
  std::span<const Id> entries; // all matches in saved order; borrows the index
  bool table_encoding_replaced = false;
};
// Classification supplied by the containing application's protocol registry.
// These are the native GetURLType values, not an inference from a URI prefix.
enum class FileUrlType { local = 0, remote = 1, unsupported = 2 };
enum class FileUrlGeneration { v2017, v2026 };
enum class FileUrlStatus {
  classified,
  invalid_url,
  unknown_protocol,
  requires_character_classification
};
struct FileProtocolRegistration {
  std::u16string scheme;
  FileUrlType type = FileUrlType::local;
};
struct FileUrlClassification {
  FileUrlStatus status = FileUrlStatus::unknown_protocol;
  FileUrlType type = FileUrlType::unsupported;
  std::u16string scheme; // owned, exact case; no percent decoding
  Id protocol = none;    // first matching registration position
  bool used_default = false;
};
// Immutable protocol snapshot. Defaults reproduce the verified local-file
// registration, not every application/plugin protocol. First duplicate wins;
// unknown nonempty schemes never use the default. Copies share the index.
class FileProtocolContext {
  struct Impl;
  std::shared_ptr<const Impl> impl;

public:
  // A non-ASCII scheme predicate must match the source runtime's combined
  // iswalpha/iswdigit classification and be safe for concurrent const calls.
  // Without it, relevant non-ASCII scheme characters remain explicitly unknown.
  explicit FileProtocolContext(
      std::vector<FileProtocolRegistration> = {{u"file", FileUrlType::local}},
      std::optional<std::u16string> default_scheme = u"file",
      std::function<bool(char16_t)> non_ascii_letter_or_digit = {});
  FileUrlClassification classify(std::optional<std::u16string_view>,
                                 FileUrlGeneration) const;
};
struct ProteinUriReadResolution {
  ExternalReadPathResolution reference;
  Id string = none;        // original ObjectGraph.strings identity
  Id embedded_file = none; // original ObjectGraph.embedded_files identity
  const EmbeddedAssetFile *embedded = nullptr; // borrows the immutable graph
  bool uses_xref = false;
  // Present only for the protocol-context overload; absent for embedded,
  // null or invalid UTF8 input. Requires-character-classification never maps.
  std::optional<FileUrlClassification> classification;
};
// Build once for a caller-selected XRef scope. Exact case-sensitive keys;
// no basename/prefix matching or filesystem access. Keep the source table
// alive and unchanged while the index and its returned views are used.
class ExternalReferenceIndex {
  struct Impl;
  std::shared_ptr<const Impl> impl;
  ProteinUriReadResolution query_protein(ProteinUriReadResolution,
                                         FileUrlType) const;

public:
  explicit ExternalReferenceIndex(const ExternalReferenceTable &);
  ExternalPathResolution resolve(const ExternalGeometry &) const;
  // UTF-8 stream read semantics: NUL termination, native conversion and last
  // matching table entry. Raw inspection above retains exact saved byte keys.
  ExternalReadPathResolution resolve_read_path(const ExternalGeometry &) const;
  // QueryXRef on an already converted native wide path; no binary stream error
  // handling or additional UTF-8 conversion. Empty/NUL paths bypass lookup.
  ExternalReadPathResolution query_path(std::u16string_view) const;
  // Read one type13 Protein property's URI in an explicitly selected graph.
  // UTF-8 stream mode only. Embedded entries bypass XRef; remote entries query
  // it; local entries query only when the native absolute-path predicate holds.
  // Supply explicit classification or a verified protocol snapshot/profile.
  // These queries do not use the host filesystem or infer shader texture roles.
  // Returned entry spans borrow this index; embedded bytes borrow the graph.
  ProteinUriReadResolution resolve_protein_uri(const ObjectGraph &, Id owner,
                                               Id property, Id ordinal,
                                               FileUrlType) const;
  ProteinUriReadResolution resolve_protein_uri(const ObjectGraph &, Id owner,
                                               Id property, Id ordinal,
                                               const FileProtocolContext &,
                                               FileUrlGeneration) const;
  // Indexed embedded lookup for bulk reads; avoids rescanning property files.
  ProteinUriReadResolution resolve_protein_uri(const ProteinGraphIndex &,
                                               Id owner, Id property,
                                               Id ordinal,
                                               const FileProtocolContext &,
                                               FileUrlGeneration) const;
};
enum class ExternalReferenceScopeStatus {
  absent,
  available,
  unavailable,
  ambiguous
};
struct ExternalReferenceScope {
  ExternalReferenceScopeStatus status = ExternalReferenceScopeStatus::absent;
  Id block = none; // file directory/ProductData.blocks index, not a model ID
  const ExternalReferenceTable *table = nullptr; // borrowed source record
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
enum class PublishOption : uint32_t {
  has_password = 1,
  display_on_open = 2,
  has_expiry_date = 16,
  has_been_resaved = 32,
  allow_resave = 64,
  display_at_password = 128,
  embed_database_properties = 512
};
struct PublishInformation {
  Id name = none, class_name = none; // objects below
  uint32_t attribute_flags = 0, flags = 0;
  std::string title, subject, author, publisher, copyright, published_for,
      comments, keywords;
  int64_t published = 0, expires = 0;
  ObjectGraph objects;
  bool has_option(PublishOption option) const {
    return (flags & static_cast<uint32_t>(option)) != 0;
  }
  uint32_t unknown_flags() const { return flags & ~uint32_t{0x2f3}; }
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
struct EmbeddedReference {
  std::u16string filename;
  std::string chunk_name; // native UTF-8 bytes, including unpaired surrogates
  bool local = false;     // original URL begins with nwd:///.#
};
// Native nwd:/// URL syntax, NUL termination and separator selection. No URL
// unescaping, filesystem lookup or implicit selection of a containing file.
std::optional<EmbeddedReference>
parse_embedded_reference(std::u16string_view path);
struct EmbeddedResourceLookup {
  Id block = none; // first matching directory entry; pass to read_resource()
  std::span<const Id> blocks; // all matching entries, borrowed from the index
};
class EmbeddedResourceIndex {
  struct Impl;
  std::shared_ptr<const Impl> impl;

public:
  // Index the explicitly selected source file. Keep chunks alive/unchanged.
  explicit EmbeddedResourceIndex(std::span<const Chunk> chunks);
  // Exact NUL-terminated byte identity, with native first-match precedence.
  EmbeddedResourceLookup find(std::string_view chunk_name) const;
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
// One root LcOpXRefTable applies to the file's streams, including named sheets.
// Prefixed tables are not substitutes. Scene lookup requires loaded products;
// ProductData lookup also supports NWF. Keep the source storage
// alive/unchanged.
ExternalReferenceScope external_reference_scope(const Scene &);
ExternalReferenceScope external_reference_scope(const ProductData &);
// Directory-only lookup reports unavailable for an existing unique root.
ExternalReferenceScope external_reference_scope(std::span<const Chunk>);
// Expands the native nwd:///.#resource form using an explicitly supplied file
// name. Performs no filesystem lookup or general path normalization.
std::u16string expand_embedded_reference_path(std::u16string_view path,
                                              std::u16string_view filename);
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
  bool resolve_external_geometry =
      true; // locate files; plugin decoding separate
  FileProtocolContext protein_protocols{};
  // Explicit URL-classifier profile; independent of file wire version.
  FileUrlGeneration protein_url_generation = FileUrlGeneration::v2026;
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
struct PropertyReference {
  Reference attribute; // source Model.graphs attribute object
  Id property = none;  // Object.properties; none denotes the category itself
};
// Borrowed source context. graphs is needed only for name-valued references.
struct SearchValueView {
  const Value &value;
  const ObjectGraph &strings;
  std::span<const ObjectGraph> graphs{};
};
struct SearchTextOptions {
  bool ignore_case = false, ignore_accents = false, ignore_widths = false;
};
struct SearchValueOptions {
  SearchTextOptions text;
  // Explicit policy: older search uses exact numeric order; newer applications
  // may use unit-dependent tolerances. A callback takes precedence over exact.
  bool exact_numeric_order = false;
  std::function<std::optional<int>(SearchValueView, SearchValueView)>
      compare_numeric;
  // Supply the application's complete text normalization when flags are used.
  // Width normalization uses full-width wildcard characters U+FF0A/U+FF1F.
  std::function<std::optional<std::u16string>(std::u16string_view,
                                              const SearchTextOptions &)>
      transform;
  uint64_t max_text_units = 1ull << 20;
  uint64_t max_match_steps = 100000000;
};
enum class SearchValueStatus {
  match,
  no_match,
  text_context_required,
  unsupported_operation,
  unsupported_value,
  numeric_context_required
};
bool search_storage_type_equal(const Value &, const Value &) noexcept;
// Evaluates the value phase (operators5..15) for an existing property.
// Category, property-name, negate, grouping and selection-scope logic are
// separate.
SearchValueStatus search_value_match(SearchOperator, SearchValueView candidate,
                                     SearchValueView target,
                                     const SearchValueOptions & = {});
struct PropertyIndexOptions {
  bool include_internal = false;
  uint64_t max_entries = 20000000;
  // Limits owned name, display-label and typed-value-key bytes, not allocator
  // overhead or allocations made inside the caller's formatter.
  uint64_t max_label_bytes = 256ull << 20;
  // Without a callback, only legacy string labels (CR/LF replaced by spaces)
  // are known. Return nullopt when the display context cannot format a value.
  std::function<std::optional<std::string>(const Model &, PropertyReference)>
      format_value;
};
enum class PropertyLocatorStatus {
  resolved,
  tree_root,
  missing,
  ambiguous,
  unsupported_root,
  display_context_required,
  unsupported_path,
  unsupported_value
};
struct PropertyLocatorResult {
  PropertyLocatorStatus status = PropertyLocatorStatus::missing;
  // Borrowed from the index; records retain source graph/property identities.
  std::span<const PropertyReference> records;
};
// Indexes directly attached serialized property attributes (types84/86).
// Application/plugin-generated property categories are not stored here.
class PropertyLocatorIndex {
  struct Impl;
  std::shared_ptr<const Impl> impl_;

public:
  explicit PropertyLocatorIndex(const Model &, PropertyIndexOptions = {});
  PropertyLocatorResult resolve(const SelectionLocatorPath &) const;
  std::span<const Id> owners(Reference attribute) const;
};
struct ProjectFileReference {
  std::string requested_path, status, diagnostic;
  std::u16string read_path;
  ExternalReadPathStatus read_status = ExternalReadPathStatus::original;
  ExternalReferenceScopeStatus scope_status =
      ExternalReferenceScopeStatus::absent;
  Id xref_block = none, xref_entry = none;
  std::vector<Id> xref_entries;
  bool source_encoding_replaced = false, table_encoding_replaced = false;
  bool uses_xref = true;
  bool caller_remapped = false;
  std::filesystem::path file; // resource container or external file
  std::optional<EmbeddedReference> embedded;
  Id resource_block = none;
  std::vector<Id> resource_blocks;
  std::optional<FileUrlClassification> protein_classification;
};
struct ExternalGeometryFile {
  Id source = none, model = none, geometry = none;
  std::shared_ptr<const ProjectFileReference> reference;
};
struct TextureFile {
  Id source = none, model = none, asset = none;
  std::string alias, requested_path, status;
  std::filesystem::path resolved_path;
  std::shared_ptr<const std::vector<uint8_t>>
      bytes; // aliasing view for embedded files
  bool active = false, thumbnail = false;
  std::shared_ptr<const ProjectFileReference> reference;
  std::optional<ProteinUriSlot>
      protein_uri;          // graph selected by source/model/asset
  bool enabled_path = true; // structural reachability, not shader evaluation
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
  // One per unique geometry record, not per instance. File resolution does not
  // imply decoding of external point-cloud/mesh plugin formats.
  std::vector<ExternalGeometryFile> external_geometry_files;
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
enum class TextStyleStatus {
  not_text,
  null_style,
  unavailable,
  wrong_type,
  resolved
};
struct TextStyleResolution {
  TextStyleStatus status = TextStyleStatus::not_text;
  const ObjectGraph *graph = nullptr;
  Id object = none;
};
// A view into model/page storage. Unavailable includes an unloaded or
// out-of-range table; raw saved identities remain in Geometry::text_fields.
TextStyleResolution resolve_text_style(const Model &, const Geometry &);
CurrentView decode_current_view(std::span<const uint8_t> data,
                                uint32_t version);
std::vector<uint32_t> triangle_indices(const Geometry &geometry);
// On-demand slot decoding: XYZ normals, RGBA colors, or UV in the first lanes.
// Short normals divide by 32767; values are not clamped or renormalized.
// Nonfinite source values and decoded results (including one-bit float normal
// palettes) are retained. No expanded vertex arrays are cached.
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

// Serialized node index, 1-based; zero means no store index. Non-node types
// return nullopt. Incomplete node records throw Error.
std::optional<uint32_t> node_guid_index(const Object &object);
// Type 180 stores the original 16 GUID bytes in Object.bytes (4-byte aligned
// on disk). Non-GUID attributes return nullopt; malformed payloads throw.
std::optional<std::array<uint8_t, 16>> guid_attribute(const Object &object);

enum class NodeGuidStatus {
  resolved,
  no_index,
  missing_store,
  unavailable_store,
  ambiguous_store,
  index_out_of_range
};
struct NodeGuidResult {
  NodeGuidStatus status = NodeGuidStatus::no_index;
  uint32_t index = 0; // original 1-based value, never a path/object ID
  Id block = none;    // Scene.products block, when uniquely identified
  const std::array<uint8_t, 16> *guid = nullptr; // borrowed, only if resolved
};
// Binds serialized node indices to the same model namespace's GuidStore.
// The Scene, its products and models must stay alive and unchanged. Duplicate
// namespaces are ambiguous. Inline partitions share the outer serialized
// model's store, including indexed roots. No GUID copies or deduplication.
// This does not execute the application's detached-node attribute fallback,
// generate GUIDs, or equate node identity with a unique path occurrence.
class NodeGuidIndex {
  struct Impl;
  std::shared_ptr<const Impl> impl_;

public:
  explicit NodeGuidIndex(const Scene &scene);
  NodeGuidIndex(Scene &&) = delete;
  NodeGuidResult resolve(Id model, Id path) const;
};
} // namespace nwd
