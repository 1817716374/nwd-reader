# RCP project reader

`RcpReader` maps an RCP archive read-only and indexes ZIP/ZIP64 members in saved order. Duplicate member names remain distinct. Raw archive and compressed member spans avoid copies. `read_entry()` decodes stored and DEFLATE members on demand and verifies size and CRC. Encrypted and unsupported compression methods remain available as raw bytes.

`RcpXmlDocument` provides a bounded ordered XML tree. It retains unknown elements, attributes, comments, processing instructions and decoded text. DTDs and external entities are not loaded.

`RcpProjectDocument` exposes project, point-cloud runtime, Scan, ShotInfo, GroupInfo and saved-view records. It preserves source strings for paths, transforms, IDs, titles and other fields. Exact unique IDs link runtime records to Scan records, ShotInfo records to point-cloud runtime and GroupInfo records, and navigator view entries to full saved-view records. Missing IDs, absent matches and duplicate identities have explicit statuses. Other project data remains accessible from the XML tree by node ID; coordinate units and rotation conventions are not inferred.

`RcpProjectReader` selects the XML member automatically when there is exactly one, or accepts an explicit member index. It resolves RCS files using exact `RcpPathRemap` entries first, then `RelativePath` beside the RCP, an absolute saved `Path`, caller-provided search directories, and filename fallback within those directories. Ambiguous matches are reported without choosing one. Missing and invalid files have separate statuses and diagnostics. External loading can be disabled. Each runtime occurrence remains distinct; repeated references to one canonical path share a read-only `RcsReader` mapping.

```cpp
#include <nwd/reader.hpp>

nwd::RcpProjectOptions options;
options.search_paths.emplace_back("external-resources");
nwd::RcpProjectReader project("survey.rcp", options);
for (const auto &reference : project.external_rcs_files()) {
  if (reference.status == nwd::RcpExternalRcsStatus::loaded) {
    // reference.rcs exposes the mapped RCS data.
  }
}
```

The library preserves project XML fields that do not yet have semantic projections. It does not assign meaning to unknown fields or infer world-coordinate composition. See also [the RCP example](../examples/rcp.cpp).
