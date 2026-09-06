#include "internal.hpp"
namespace nwd::detail {
struct Ref {
  uint32_t type = 0;
  Id index = none;
};
static uint64_t transform_hash(const Transform &t) {
  uint64_t h = 1469598103934665603ull;
  h = (h ^ t.type) * 1099511628211ull;
  for (auto value : t.values) {
    uint64_t bits;
    std::memcpy(&bits, &value, 8);
    h = (h ^ bits) * 1099511628211ull;
  }
  return h;
}
class Fragments {
  Cursor r;
  Model &m;
  uint32_t version;
  const Options &options;
  std::vector<Ref> references;
  std::unordered_multimap<uint64_t, Id> transforms;
  uint32_t depth = 0;
  Ref object() {
    require(++depth < 128, "fragment object recursion limit");
    struct Depth {
      uint32_t &d;
      ~Depth() { --d; }
    } depth_guard{depth};
    uint32_t id = r.u32();
    if (!id)
      return {};
    if (id == 1) {
      id = r.u32();
      require(id < references.size() && references[id].type != 0,
              "missing object reference");
      return references[id];
    }
    if (id < 100 || id >= options.max_objects)
      r.fail("object ID limit: " + std::to_string(id));
    if (id >= references.size())
      references.resize(id + 1);
    require(references[id].type == 0, "duplicate object ID");
    uint32_t type = r.u32();
    Ref ref{type, none};
    if (type == 13) {
      Instance x;
      x.precision = r.f64();
      x.primitive_count = r.u32();
      x.bits = r.u32();
      x.flags = r.u32();
      x.path = r.u32();
      auto t = object();
      require(!t.type || (t.type >= 14 && t.type <= 17),
              "shape transform type");
      x.transform = t.index;
      auto a = object();
      require(!a.type || a.type == 55, "shape appearance type");
      x.appearance = a.index;
      // The native reader translates serialized bit 29 to internal bit 17.
      // Testing bit 17 in the on-disk flags misses the optional record.
      if (version >= 243 && (x.flags & 0x20000080u) == 0x20000080u) {
        AuxiliaryTransform saved;
        saved.type = r.u32();
        require(saved.type <= 2, "unknown auxiliary shape transform type");
        const unsigned count = saved.type == 0 ? 12 : saved.type == 1 ? 3 : 7;
        for (unsigned i = 0; i < count; ++i)
          saved.values[i] = r.f64();
        if (saved.type == 0) {
          auto flag = r.u32();
          require(flag <= 1, "invalid auxiliary transform orientation");
          saved.orientation = flag != 0;
        }
        x.auxiliary_transform = static_cast<Id>(m.auxiliary_transforms.size());
        m.auxiliary_transforms.push_back(saved);
      }
      auto g = object();
      require(g.type == 93, "shape geometry reference type");
      x.geometry_reference = g.index;
      ref.index = static_cast<Id>(m.instances.size());
      m.instances.push_back(x);
    } else if (type >= 14 && type <= 17) {
      Transform t;
      t.type = type;
      if (type == 17) {
        for (unsigned i = 0; i < 13; ++i)
          t.values[i] = r.f32();
        for (unsigned i = 13; i < 16; ++i)
          t.values[i] = r.f64();
      } else {
        unsigned n = type == 14 ? 3 : type == 15 ? 7 : 8;
        for (unsigned i = 0; i < n; ++i)
          t.values[i] = r.f64();
      }
      ++m.source_transform_count;
      if (options.intern_transforms) {
        auto h = transform_hash(t);
        auto range = transforms.equal_range(h);
        for (auto i = range.first; i != range.second; ++i) {
          auto &existing = m.transforms[i->second];
          if (existing.type == t.type &&
              std::memcmp(existing.values.data(), t.values.data(),
                          sizeof(t.values)) == 0) {
            ref.index = i->second;
            break;
          }
        }
        if (ref.index == none) {
          ref.index = static_cast<Id>(m.transforms.size());
          m.transforms.push_back(t);
          transforms.emplace(h, ref.index);
        }
      } else {
        ref.index = static_cast<Id>(m.transforms.size());
        m.transforms.push_back(t);
      }
    } else if (type == 54) {
      Material material;
      for (auto &v : material.values)
        v = r.f32();
      ref.index = static_cast<Id>(m.materials.size());
      m.materials.push_back(material);
    } else if (type == 55) {
      auto a = read_appearance(r, [&](uint32_t type) {
        auto child = object();
        require(child.type == type, "appearance child type");
        return child.index;
      });
      ref.index = static_cast<Id>(m.appearances.size());
      m.appearances.push_back(a);
    } else if (type == 185) {
      Asset a;
      a.json = r.string();
      if (version >= 447)
        a.extra = r.string();
      uint32_t n = r.u32();
      require(n < 1000000, "asset file count");
      for (uint32_t i = 0; i < n; ++i) {
        auto name = r.string();
        auto embedded = r.u32();
        require(embedded <= 1, "invalid asset embedded flag");
        if (embedded)
          a.embedded_files.push_back(
              read_embedded_asset(r, std::move(name), none, i));
        else
          a.files.emplace_back(name, r.string());
      }
      ref.index = static_cast<Id>(m.assets.size());
      m.assets.push_back(std::move(a));
    } else if (type == 93) {
      GeometryReference g;
      for (auto &v : g.bounds)
        v = r.f32();
      for (auto &v : g.origin)
        v = r.f64();
      g.tolerance = r.f32();
      g.checksum = r.u32();
      uint32_t gid = r.u32();
      require(gid >= 1 && gid <= m.geometries.size(),
              "geometry ID outside record table");
      g.geometry = gid - 1;
      ref.index = static_cast<Id>(m.geometry_references.size());
      m.geometry_references.push_back(g);
    } else
      r.fail("unknown fragment object type " + std::to_string(type));
    references[id] = ref;
    return ref;
  }

public:
  Fragments(Model &model, std::span<const uint8_t> bytes, uint32_t v,
            const Options &o)
      : r(bytes, "fragments"), m(model), version(v), options(o) {}
  void read() {
    uint32_t count = r.u32();
    require(count <= options.max_objects, "fragment count resource limit");
    m.instances.reserve(count);
    references.reserve(
        std::min<uint64_t>(options.max_objects, uint64_t(count) * 2 + 100));
    for (uint32_t i = 0; i < count; ++i) {
      size_t before = m.instances.size();
      auto ref = object();
      require(ref.type == 13, "expected shape object");
      if (before == m.instances.size())
        m.instances.push_back(m.instances[ref.index]);
    }
    r.exact();
    require(m.instances.size() == count, "fragment output count mismatch");
  }
  void read_overrides(NwfData &data) {
    auto n = r.u32();
    require(n <= options.max_objects, "appearance override count limit");
    for (uint32_t i = 0; i < n; ++i) {
      auto a = object();
      require(a.type == 55, "NWF override must be appearance");
      AppearanceSelection s;
      s.appearance = a.index;
      auto count = r.u32();
      require(count <= options.max_objects, "override path count limit");
      for (uint32_t j = 0; j < count; ++j)
        s.paths.push_back(r.u32());
      data.appearance_overrides.push_back(std::move(s));
    }
    if (version >= 438) {
      auto a = object();
      require(!a.type || a.type == 185, "unsupported NWF global asset");
      data.global_asset = a.index;
    }
    r.exact();
  }
};
void read_instances(Model &m, std::span<const uint8_t> data, uint32_t version,
                    const Options &o) {
  Fragments(m, data, version, o).read();
}
void read_nwf_appearances(NwfData &n, std::span<const uint8_t> data,
                          uint32_t version, const Options &o) {
  Fragments(n.appearance_values, data, version, o).read_overrides(n);
}
} // namespace nwd::detail
namespace nwd {
std::array<double, 16> world_matrix(const Model &m, const Instance &instance) {
  using namespace detail;
  std::array<double, 16> out{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if (instance.transform != none) {
    require(instance.transform < m.transforms.size(),
            "invalid transform handle");
    auto &t = m.transforms[instance.transform];
    require(t.type >= 14 && t.type <= 17, "unsupported transform type");
    auto &v = t.values;
    if (t.type == 17) {
      for (unsigned i = 0; i < 12; ++i)
        out[i] = v[i];
      for (unsigned i = 0; i < 3; ++i)
        out[12 + i] = v[13 + i];
      out[15] = v[12];
    } else {
      for (unsigned j = 0; j < 3; ++j)
        out[12 + j] = v[j];
      if (t.type >= 15) {
        double x = v[3], y = v[4], z = v[5], w = v[6],
               s = t.type == 16 ? v[7] : 1;
        require(std::abs(x * x + y * y + z * z + w * w - 1) < 1e-5,
                "non-unit rotation quaternion");
        out[0] = (1 - 2 * (y * y + z * z)) * s;
        out[1] = 2 * (x * y + z * w) * s;
        out[2] = 2 * (x * z - y * w) * s;
        out[4] = 2 * (x * y - z * w) * s;
        out[5] = (1 - 2 * (x * x + z * z)) * s;
        out[6] = 2 * (y * z + x * w) * s;
        out[8] = 2 * (x * z + y * w) * s;
        out[9] = 2 * (y * z - x * w) * s;
        out[10] = (1 - 2 * (x * x + y * y)) * s;
      }
    }
  } else if (instance.flags & 0x40000000) {
    require(instance.geometry_reference < m.geometry_references.size(),
            "invalid geometry reference");
    for (unsigned j = 0; j < 3; ++j)
      out[12 + j] =
          m.geometry_references[instance.geometry_reference].origin[j];
  }
  return out;
}
} // namespace nwd
