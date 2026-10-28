# RCP project reader

`RcpReader` maps an RCP archive read-only and indexes ZIP/ZIP64 members in central-directory order. Duplicate member names remain distinct. `raw_archive()` and `raw_compressed_entry()` expose borrowed byte spans. `read_entry()` decodes stored and DEFLATE members on demand and verifies the uncompressed size and CRC.

Entry names are exposed as UTF-8 in `RcpArchiveEntry::name`. The reader honors the ZIP UTF-8 flag; when it is unset, it accepts a version 1 Info-ZIP Unicode Path extra field only when its CRC matches the original filename bytes, then falls back to CP437. `raw_name` preserves the exact filename bytes and `name_encoding` reports the selected branch. An undefined legacy language-encoding field is reported as `unknown_legacy_encoding`; the displayed name then uses the CP437 fallback. These rules follow the ZIP format specification's filename encoding and Unicode Path definitions. [PKWARE APPNOTE](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT)

`RcpXmlDocument` provides a bounded, ordered XML tree. It retains unknown elements, attributes, comments, processing instructions and decoded text. DTDs and external entities are not loaded.

`RcpProjectDocument` exposes project UUID, point-cloud runtime, Scan, ShotInfo, GroupInfo, navigator view and full saved-view records. It retains source strings for paths, transforms, IDs, titles and other projected fields. Exact unique IDs link runtime records to Scan records, ShotInfo records to point-cloud runtime and GroupInfo records, and navigator view entries to full saved-view records. Missing IDs, absent matches and duplicate IDs have explicit statuses. Other XML fields remain accessible by node ID; units and transform conventions are not inferred.

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
