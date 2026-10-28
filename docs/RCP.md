# RCP Reader

`RcpReader` opens an RCP archive with a read-only memory map and indexes its
ZIP central directory. Entries remain in saved order, so duplicate names stay
distinct. The archive and compressed member bytes can be borrowed without a
copy. `read_entry()` decodes stored and DEFLATE members on demand and validates
their CRC. ZIP64 member sizes and offsets are supported.

```cpp
#include <nwd/reader.hpp>

nwd::RcpReader archive("project.rcp");
for (uint32_t i = 0; i < archive.entries().size(); ++i) {
  const auto &entry = archive.entries()[i];
  if (entry.name.ends_with(".xml")) {
    auto bytes = archive.read_entry(i);
    nwd::RcpProjectDocument project(bytes);
    for (const auto &item : project.voxel_tree_runtime()) {
      // item.relative_path and item.scan_index preserve source identity.
    }
  }
}
```

`RcpXmlDocument` exposes an ordered XML tree with element/attribute lookup and
text extraction. It retains unknown elements and attributes, comments, and
processing instructions under configurable byte, node, attribute, and depth
limits. DTDs and external entities are not loaded.

`RcpProjectDocument` provides source-named fields for the project UUID,
`VoxelTreeRunTime`, and `Scan` records. Translation, rotation, scale, path,
point-count, and visibility values remain their saved strings; coordinate
units and rotation conventions are not inferred. Runtime records and Scan
records are linked only when their IDs match exactly and uniquely. Missing or
duplicate identities have explicit statuses. All other XML remains
accessible by source node ID through `xml()`.

The reader decodes stored and DEFLATE compression. Encrypted members and
unknown compression methods are not decoded, but their raw compressed bytes
remain available. Automatic lookup of external RCS paths is not part of
`RcpReader`; callers can use the retained `RelativePath` values to locate those
files and open them with `RcsReader`.
