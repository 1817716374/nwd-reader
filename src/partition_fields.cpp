#include "internal.hpp"

namespace nwd {
PartitionOrientation partition_orientation_fields(const Object &object,
                                                  uint32_t version) {
  detail::require(object.type == 32, "orientation fields require a partition");
  detail::require(version >= 53,
                  "partition directions before version53 absent");
  const size_t count = version >= 230 ? 16 : version >= 109 ? 12 : 9;
  detail::require(object.numbers.size() == count,
                  "inconsistent partition orientation payload");
  PartitionOrientation result;
  std::copy_n(object.numbers.begin() + 3, 3, result.up.begin());
  std::copy_n(object.numbers.begin() + 6, 3, result.north.begin());
  if (version >= 109) {
    auto &front = result.front.emplace();
    std::copy_n(object.numbers.begin() + 9, 3, front.begin());
  }
  return result;
}
} // namespace nwd
