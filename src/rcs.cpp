#include "nwd/reader.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nwd {
namespace {
constexpr uint64_t header_bytes = 0x1812d;
constexpr uint64_t chunk_count_offset = 0x129;
constexpr uint64_t chunk_table_offset = 0x12d;
constexpr uint64_t chunk_entry_bytes = 24;
constexpr uint64_t voxel_count_bytes = 8;
constexpr uint64_t umbrella_octree_header_bytes = 0x80;
constexpr uint64_t voxel_record_bytes = RcsVoxelDescriptor::wire_size;
constexpr uint64_t umbrella_octree_record_bytes =
    RcsFileIndex::umbrella_octree_record_size;
constexpr uint32_t point_record_bytes = 16;
constexpr uint32_t normal_grid_width = 52;
constexpr uint32_t normal_face_entries = normal_grid_width * normal_grid_width;
constexpr uint32_t normal_entry_count = 6 * normal_face_entries;

void range(std::span<const uint8_t> bytes, uint64_t offset, uint64_t size,
           const char *what) {
  if (offset > bytes.size() || size > bytes.size() - offset)
    throw Error(std::string("RCS ") + what + " outside file");
}

uint32_t u32(std::span<const uint8_t> bytes, uint64_t offset) {
  range(bytes, offset, 4, "u32");
  return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
         (uint32_t(bytes[offset + 2]) << 16) |
         (uint32_t(bytes[offset + 3]) << 24);
}

uint64_t u64(std::span<const uint8_t> bytes, uint64_t offset) {
  return uint64_t(u32(bytes, offset)) | (uint64_t(u32(bytes, offset + 4)) << 32);
}

double f64(std::span<const uint8_t> bytes, uint64_t offset) {
  return std::bit_cast<double>(u64(bytes, offset));
}

const std::array<std::array<float, 3>, normal_entry_count> &rcs_normal_table() {
  static const auto table = [] {
    std::array<std::array<float, 3>, normal_entry_count> result{};
    std::array<float, 26> tangent{};
    constexpr float pi = 3.1415927410125732421875f;
    for (uint32_t i = 0; i < tangent.size(); ++i) {
      const float half_degree_sample = static_cast<float>(i) + 0.5f;
      tangent[i] = std::tan(half_degree_sample * pi / 104.0f);
    }

    for (uint32_t face = 0; face < 6; ++face) {
      for (uint32_t row = 0; row < normal_grid_width; ++row) {
        const bool row_positive = row >= 26;
        const uint32_t row_magnitude = row_positive ? row - 26 : 25 - row;
        const float outer = tangent[row_magnitude];
        for (uint32_t column = 0; column < normal_grid_width; ++column) {
          const bool column_positive = column >= 26;
          const uint32_t column_magnitude =
              column_positive ? column - 26 : 25 - column;
          const float inner = tangent[column_magnitude];
          const float norm = std::sqrt(outer * outer + inner * inner + 1.0f);
          const float axis = 1.0f / norm;
          float row_component = outer / norm;
          float column_component = inner / norm;
          if (!row_positive)
            row_component = -row_component;
          if (!column_positive)
            column_component = -column_component;

          std::array<float, 3> normal{};
          switch (face) {
          case 0: normal = {axis, column_component, row_component}; break;
          case 1: normal = {-axis, column_component, row_component}; break;
          case 2: normal = {column_component, axis, row_component}; break;
          case 3: normal = {column_component, -axis, row_component}; break;
          case 4: normal = {column_component, row_component, axis}; break;
          default: normal = {column_component, row_component, -axis}; break;
          }
          result[face * normal_face_entries + row * normal_grid_width +
                 column] = normal;
        }
      }
    }
    return result;
  }();
  return table;
}

uint64_t sum_words(std::span<const uint8_t> file, uint64_t offset,
                   uint64_t count) {
  uint64_t sum = 0;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t value = u32(file, offset + i * 4);
    if (value > std::numeric_limits<uint64_t>::max() - sum)
      throw Error("RCS descriptor count overflow");
    sum += value;
  }
  return sum;
}

const RcsChunk *first_chunk(const RcsFileIndex &index, uint32_t id) {
  for (const auto &chunk : index.chunks)
    if (chunk.id == id)
      return &chunk;
  return nullptr;
}
} // namespace

RcsFileIndex read_rcs_index(std::span<const uint8_t> file,
                            uint64_t max_voxels) {
  range(file, 0, header_bytes, "fixed header");
  if (std::memcmp(file.data(), "ADOCT", 5) != 0)
    throw Error("RCS magic is not ADOCT");

  RcsFileIndex result;
  result.version_major = u32(file, 8);
  result.version_minor = u32(file, 12);
  if (!((result.version_major == 3 && result.version_minor >= 1) ||
        (result.version_major == 4 && result.version_minor == 0)))
    throw Error("unsupported RCS file version");

  uint32_t chunk_count = u32(file, chunk_count_offset);
  if (chunk_count > 0x1000)
    throw Error("RCS chunk count exceeds native limit");
  uint64_t table_size = uint64_t(chunk_count) * chunk_entry_bytes;
  range(file.first(static_cast<size_t>(header_bytes)), chunk_table_offset,
        table_size, "chunk table");
  result.chunks.reserve(chunk_count);
  for (uint32_t i = 0; i < chunk_count; ++i) {
    uint64_t at = chunk_table_offset + uint64_t(i) * chunk_entry_bytes;
    RcsChunk chunk{u32(file, at), u32(file, at + 4), u64(file, at + 8),
                   u64(file, at + 16)};
    range(file, chunk.offset, chunk.size, "chunk");
    result.chunks.push_back(chunk);
  }

  if (const RcsChunk *umbrella_chunk = first_chunk(result, 3)) {
    if (umbrella_chunk->size < umbrella_octree_header_bytes)
      throw Error("RCS umbrella octree chunk has no level count table");
    uint64_t record_count = 0;
    for (uint32_t level = 0;
         level < result.umbrella_octree_counts.size(); ++level) {
      const uint32_t count = u32(file, umbrella_chunk->offset + level * 4);
      result.umbrella_octree_counts[level] = count;
      if (count > std::numeric_limits<uint64_t>::max() - record_count)
        throw Error("RCS umbrella octree record count overflow");
      record_count += count;
    }
    if (record_count >
        (std::numeric_limits<uint64_t>::max() - umbrella_octree_header_bytes) /
            umbrella_octree_record_bytes)
      throw Error("RCS umbrella octree data size overflow");
    const uint64_t records_bytes =
        record_count * umbrella_octree_record_bytes;
    if (records_bytes > umbrella_chunk->size - umbrella_octree_header_bytes)
      throw Error("RCS umbrella octree records exceed their chunk");
    result.umbrella_octree_records_offset =
        umbrella_chunk->offset + umbrella_octree_header_bytes;
    result.umbrella_octree_record_count = record_count;
  }

  const RcsChunk *voxel_chunk = first_chunk(result, 2);
  const RcsChunk *point_chunk = first_chunk(result, 1);
  if (!voxel_chunk || !point_chunk)
    throw Error("RCS voxel or point-data chunk is missing");
  if (voxel_chunk->size < voxel_count_bytes)
    throw Error("RCS voxel chunk has no descriptor count");
  uint64_t count = u64(file, voxel_chunk->offset);
  if (count > max_voxels || count > std::numeric_limits<size_t>::max())
    throw Error("RCS voxel descriptor count exceeds configured limit");
  if (count > (std::numeric_limits<uint64_t>::max() - voxel_count_bytes) /
                  voxel_record_bytes)
    throw Error("RCS voxel descriptor table size overflow");
  uint64_t descriptor_bytes = voxel_count_bytes + count * voxel_record_bytes;
  if (descriptor_bytes > voxel_chunk->size)
    throw Error("RCS voxel descriptor table exceeds its chunk");

  result.voxels.reserve(static_cast<size_t>(count));
  uint64_t segment_offset = 0;
  for (uint64_t i = 0; i < count; ++i) {
    const uint64_t at = voxel_chunk->offset + voxel_count_bytes +
                        i * voxel_record_bytes;
    RcsVoxelDescriptor voxel;
    voxel.record_offset = at;
    for (uint64_t j = 0; j < 6; ++j) {
      voxel.primary_bounds[j] = f64(file, at + j * 8);
      voxel.secondary_bounds[j] = f64(file, at + 0x30 + j * 8);
    }
    voxel.unknown_word_60 = u32(file, at + 0x60);
    voxel.point_count = u32(file, at + 0x64);
    voxel.data_span_bytes = u32(file, at + 0x68);
    for (uint64_t j = 0; j < voxel.auxiliary_counts.size(); ++j)
      voxel.auxiliary_counts[j] = u32(file, at + 0x6c + j * 4);
    for (uint64_t j = 0; j < voxel.lod_point_counts.size(); ++j)
      voxel.lod_point_counts[j] = u32(file, at + 0xcc + j * 4);
    for (uint64_t j = 0; j < voxel.unknown_tail.size(); ++j)
      voxel.unknown_tail[j] = u32(file, at + 0x14c + j * 4);

    uint64_t aux_count = sum_words(file, at + 0x6c, 24);
    uint64_t lod_count = sum_words(file, at + 0xcc, 32);
    if (lod_count != voxel.point_count)
      throw Error("RCS voxel LOD counts do not match descriptor point count");
    uint64_t prefix_items = aux_count + lod_count;
    if (prefix_items > (std::numeric_limits<uint64_t>::max() -
                        uint64_t(lod_count) * point_record_bytes) /
                           4)
      throw Error("RCS voxel point span overflow");
    uint64_t derived_span = prefix_items * 4 + lod_count * point_record_bytes;
    if (derived_span != voxel.data_span_bytes)
      throw Error("RCS voxel data span does not match count tables");
    if (segment_offset > point_chunk->size ||
        derived_span > point_chunk->size - segment_offset)
      throw Error("RCS voxel data segments exceed point-data chunk");
    if (point_chunk->offset > std::numeric_limits<uint64_t>::max() -
                                  segment_offset - prefix_items * 4)
      throw Error("RCS point-data file offset overflow");
    voxel.point_data_offset =
        point_chunk->offset + segment_offset + prefix_items * 4;
    uint64_t point_bytes = lod_count * point_record_bytes;
    range(file, voxel.point_data_offset, point_bytes, "voxel point records");
    segment_offset += derived_span;
    result.voxels.push_back(std::move(voxel));
  }

  if (const RcsChunk *timestamp_chunk = first_chunk(result, 13)) {
    if (timestamp_chunk->size < 4)
      throw Error("RCS timestamp chunk has no point count");
    result.has_timestamp_chunk = true;
    result.timestamp_count = u32(file, timestamp_chunk->offset);
    uint64_t point_total = 0;
    for (const auto &voxel : result.voxels) {
      if (voxel.point_count >
          std::numeric_limits<uint64_t>::max() - point_total)
        throw Error("RCS timestamp point count overflow");
      point_total += voxel.point_count;
    }
    result.timestamp_count_matches_points = result.timestamp_count == point_total;
    if (point_total >
        (std::numeric_limits<uint64_t>::max() - 4) / sizeof(double))
      throw Error("RCS timestamp data size overflow");
    const uint64_t timestamp_bytes = point_total * sizeof(double);
    if (timestamp_bytes > timestamp_chunk->size - 4)
      throw Error("RCS timestamp values exceed their chunk");
    uint64_t timestamp_offset = timestamp_chunk->offset + 4;
    for (auto &voxel : result.voxels) {
      voxel.timestamp_data_offset = timestamp_offset;
      timestamp_offset += uint64_t(voxel.point_count) * sizeof(double);
    }
  }
  return result;
}

std::array<float, 3> decode_rcs_normal(uint16_t normal_index) {
  if (normal_index >= normal_entry_count)
    return {};
  return rcs_normal_table()[normal_index];
}

RcsRawPoint decode_rcs_point(std::span<const uint8_t> file,
                             const RcsVoxelDescriptor &voxel,
                             uint32_t point_index) {
  if (point_index >= voxel.point_count)
    throw Error("RCS point index outside voxel");
  const uint64_t delta = uint64_t(point_index) * point_record_bytes;
  if (voxel.point_data_offset > std::numeric_limits<uint64_t>::max() - delta)
    throw Error("RCS point offset overflow");
  uint64_t offset = voxel.point_data_offset + delta;
  range(file, offset, point_record_bytes, "raw point");
  RcsRawPoint point;
  point.raw_position_word = u64(file, offset);
  point.raw_attribute_word = u64(file, offset + 8);
  const uint64_t packed = point.raw_position_word;
  const std::array<uint32_t, 3> quantized{
      static_cast<uint32_t>(packed & 0x3ffff),
      static_cast<uint32_t>((packed >> 18) & 0x3ffff),
      static_cast<uint32_t>((packed >> 36) & 0x3ffff)};
  for (size_t axis = 0; axis < 3; ++axis)
    point.position[axis] =
        voxel.primary_bounds[axis] +
        static_cast<double>(static_cast<float>(quantized[axis])) * 0.001;
  point.lidar_classification = static_cast<uint8_t>((packed >> 54) & 0xff);
  point.deleted = (packed >> 63) != 0;
  for (size_t i = 0; i < point.rgba.size(); ++i)
    point.rgba[i] = file[offset + 8 + i];
  point.normal = static_cast<uint16_t>((point.raw_attribute_word >> 32) & 0x3fff);
  point.primitive_classification =
      static_cast<uint8_t>((point.raw_attribute_word >> 46) & 0x0f);
  return point;
}

struct RcsReader::Impl {
  const uint8_t *data = nullptr;
  size_t size = 0;
  RcsFileIndex index;
#ifdef _WIN32
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping = nullptr;
#else
  int file = -1;
#endif

  ~Impl() {
#ifdef _WIN32
    if (data)
      UnmapViewOfFile(data);
    if (mapping)
      CloseHandle(mapping);
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
#else
    if (data)
      munmap(const_cast<uint8_t *>(data), size);
    if (file >= 0)
      close(file);
#endif
  }

  void open(const std::filesystem::path &path) {
#ifdef _WIN32
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      throw Error("cannot open RCS file");
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0 ||
        static_cast<uint64_t>(length.QuadPart) >
            std::numeric_limits<size_t>::max())
      throw Error("invalid RCS file size");
    size = static_cast<size_t>(length.QuadPart);
    mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping)
      throw Error("cannot map RCS file");
    data = static_cast<const uint8_t *>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!data)
      throw Error("cannot map RCS file view");
#else
    file = ::open(path.c_str(), O_RDONLY);
    if (file < 0)
      throw Error("cannot open RCS file");
    struct stat info {};
    if (fstat(file, &info) != 0 || info.st_size <= 0 ||
        static_cast<uint64_t>(info.st_size) >
            std::numeric_limits<size_t>::max())
      throw Error("invalid RCS file size");
    size = static_cast<size_t>(info.st_size);
    void *view = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file, 0);
    if (view == MAP_FAILED)
      throw Error("cannot map RCS file");
    data = static_cast<const uint8_t *>(view);
#endif
  }
};

RcsReader::RcsReader(const std::filesystem::path &path, uint64_t max_voxels) {
  auto impl = std::make_shared<Impl>();
  impl->open(path);
  impl->index = read_rcs_index({impl->data, impl->size}, max_voxels);
  impl_ = std::move(impl);
}

const RcsFileIndex &RcsReader::index() const noexcept { return impl_->index; }

std::span<const uint8_t> RcsReader::raw_points(uint32_t voxel_index) const {
  if (voxel_index >= impl_->index.voxels.size())
    throw Error("RCS voxel index outside descriptor table");
  const auto &voxel = impl_->index.voxels[voxel_index];
  uint64_t size = uint64_t(voxel.point_count) * point_record_bytes;
  range({impl_->data, impl_->size}, voxel.point_data_offset, size,
        "voxel point records");
  return {impl_->data + voxel.point_data_offset, static_cast<size_t>(size)};
}

std::array<std::span<const uint8_t>,
           RcsVoxelDescriptor::auxiliary_table_count>
RcsReader::raw_auxiliary_tables(uint32_t voxel_index) const {
  if (voxel_index >= impl_->index.voxels.size())
    throw Error("RCS voxel index outside descriptor table");

  const auto &voxel = impl_->index.voxels[voxel_index];
  uint64_t auxiliary_total = 0;
  for (const uint32_t count : voxel.auxiliary_counts)
    auxiliary_total += count;
  const uint64_t prefix_items = auxiliary_total + voxel.point_count;
  if (prefix_items > std::numeric_limits<uint64_t>::max() / 4 ||
      voxel.point_data_offset < prefix_items * 4)
    throw Error("RCS auxiliary table offset underflow");
  uint64_t offset = voxel.point_data_offset - prefix_items * 4;
  std::array<std::span<const uint8_t>,
             RcsVoxelDescriptor::auxiliary_table_count> result{};
  const std::span<const uint8_t> file{impl_->data, impl_->size};
  for (size_t i = 0; i < result.size(); ++i) {
    const uint64_t size = uint64_t(voxel.auxiliary_counts[i]) * 4;
    range(file, offset, size, "auxiliary table");
    result[i] = {impl_->data + offset, static_cast<size_t>(size)};
    offset += size;
  }
  return result;
}

std::array<std::span<const uint8_t>,
           RcsFileIndex::umbrella_octree_level_count>
RcsReader::raw_umbrella_octree_levels() const {
  std::array<std::span<const uint8_t>,
             RcsFileIndex::umbrella_octree_level_count> result{};
  const auto &index = impl_->index;
  if (index.umbrella_octree_record_count == 0)
    return result;

  uint64_t offset = index.umbrella_octree_records_offset;
  const std::span<const uint8_t> file{impl_->data, impl_->size};
  for (size_t level = 0; level < result.size(); ++level) {
    const uint64_t size =
        uint64_t(index.umbrella_octree_counts[level]) *
        RcsFileIndex::umbrella_octree_record_size;
    range(file, offset, size, "umbrella octree level");
    result[level] = {impl_->data + offset, static_cast<size_t>(size)};
    offset += size;
  }
  return result;
}

std::span<const uint8_t>
RcsReader::raw_timestamps(uint32_t voxel_index) const {
  if (voxel_index >= impl_->index.voxels.size())
    throw Error("RCS voxel index outside descriptor table");
  if (!impl_->index.has_timestamp_chunk)
    return {};
  const auto &voxel = impl_->index.voxels[voxel_index];
  const uint64_t size = uint64_t(voxel.point_count) * sizeof(double);
  range({impl_->data, impl_->size}, voxel.timestamp_data_offset, size,
        "voxel timestamps");
  return {impl_->data + voxel.timestamp_data_offset,
          static_cast<size_t>(size)};
}

double RcsReader::timestamp(uint32_t voxel_index,
                            uint32_t point_index) const {
  if (voxel_index >= impl_->index.voxels.size())
    throw Error("RCS voxel index outside descriptor table");
  if (!impl_->index.has_timestamp_chunk)
    throw Error("RCS timestamp chunk is absent");
  const auto &voxel = impl_->index.voxels[voxel_index];
  if (point_index >= voxel.point_count)
    throw Error("RCS timestamp point index outside voxel");
  const uint64_t offset = voxel.timestamp_data_offset +
                          uint64_t(point_index) * sizeof(double);
  return f64({impl_->data, impl_->size}, offset);
}

RcsRawPoint RcsReader::point(uint32_t voxel_index,
                             uint32_t point_index) const {
  if (voxel_index >= impl_->index.voxels.size())
    throw Error("RCS voxel index outside descriptor table");
  return decode_rcs_point({impl_->data, impl_->size},
                          impl_->index.voxels[voxel_index], point_index);
}
} // namespace nwd
