# RCP project reader

`RcpReader` maps an RCP archive read-only and indexes ZIP/ZIP64 members in central-directory order. Duplicate member names remain distinct. `raw_archive()` and `raw_compressed_entry()` expose borrowed byte spans. `read_entry()` decodes stored and DEFLATE members on demand and verifies the uncompressed size and CRC.

Entry names and central-directory comments are exposed as UTF-8 in `RcpArchiveEntry::name` and `comment`. The reader honors the ZIP UTF-8 flag; when it is unset, it accepts a version 1 Info-ZIP Unicode Path (`0x7075`) or Unicode Comment (`0x6375`) extra field only when its CRC matches the original filename or comment bytes, then falls back to CP437. `raw_name` preserves the exact filename bytes, `central_comment` preserves the exact comment bytes, and `name_encoding`/`comment_encoding` report the selected branch. An undefined legacy language-encoding field is reported as `unknown_legacy_encoding`; the displayed text then uses the CP437 fallback. These rules follow the ZIP format specification's filename/comment encoding and Unicode extra field definitions. [PKWARE APPNOTE](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT)

`RcpXmlDocument` provides a bounded, ordered XML tree. It retains unknown elements, attributes, comments, processing instructions and decoded text. DTDs and external entities are not loaded.

`RcpProjectDocument` exposes project UUID, point-cloud runtime, Scan, ShotInfo, GroupInfo, navigator view and full saved-view records. It retains source strings for paths, transforms, IDs, titles and other projected fields. Saved views expose raw `FOV`, `PointSize`, and `ShadingMode` strings, plus XML node IDs for their Camera, LimitBox, ClippingBoundaries, Regions, DefaultRegionStatus, ScanLocations, and BackgroundColor children. Camera `Eye`, `Center`, and `Up` fields expose the original lowercase `x/y/z` and uppercase `X/Y/Z` attribute strings separately; the reader does not merge the variants, convert them to floating point, or assign units. Each `ScanLocations/Status` retains its source ID and `Visible` string and links to a `ProjectNavigator/Scans/Scan` only when the ID is unique on both sides. `RegionManager/DefaultLayer` and `RegionArray` layer records preserve raw IDs, names, indexes, visibility/lock/removal strings, and Color/SelectionBounds node IDs. Each saved-view `DefaultRegionStatus/Status` retains its ID/visibility and links to a region layer only for a unique ID; `Regions/@Isolated` and nested statuses remain source values. Exact unique IDs also link runtime records to Scan records, ShotInfo records to point-cloud runtime and GroupInfo records, and navigator view entries to full saved-view records. Missing IDs, absent matches and duplicate IDs have explicit statuses. Other XML fields remain accessible by node ID; FOV units and transform conventions are not inferred.

`RcpProjectReader` selects the only XML member automatically or accepts an explicit member index. It resolves external RCS references in this order: exact `RcpPathRemap`, `RelativePath` beside the RCP, absolute saved `Path`, caller-provided search directories, then filename-only candidates in those directories. It reports ambiguous, missing and invalid files separately, and external loading can be disabled. Each runtime occurrence stays distinct; references to the same canonical file share one read-only `RcsReader` mapping.

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

Encrypted members and compression methods other than stored and DEFLATE are not decoded. Their source bytes and unknown project XML remain accessible.
