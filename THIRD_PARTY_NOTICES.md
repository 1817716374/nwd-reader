# Third-party components

This library bundles the following source dependencies. Their copyright notices and license texts are retained with the source.

| Component | Version | Purpose | License |
|---|---|---|---|
| [zlib](https://github.com/madler/zlib/tree/v1.3.1) | 1.3.1 | Block decompression | [zlib license](third_party/zlib-1.3.1/LICENSE) |
| [JSON for Modern C++](https://github.com/nlohmann/json/tree/v3.11.3) | 3.11.3 | Reference tables and material assets | [MIT license](third_party/json-LICENSE.MIT) |
| [SQLite](https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip) | 3.53.4 | In-memory embedded database reading | [Public domain](third_party/sqlite/LICENSE) |
| [Go cryptography Blowfish constants](https://github.com/golang/crypto/tree/master/blowfish) | Vendored constants | Format cipher initialization | [BSD 3-Clause](third_party/blowfish-LICENSE) |

| [tiny-AES-c](https://github.com/kokke/tiny-AES-c/tree/23856752fbd139da0b8ca6e471a13d5bcc99a08d) | `23856752fbd1` | LightWorks AES128 decoding | [Unlicense](third_party/tiny-aes/unlicense.txt) |
| [ISAAC32](https://burtleburtle.net/bob/c/rand.c) | Algorithm reference | Archive default key derivation | Public domain |

Only the zlib source files and build inputs needed by this project are bundled. Upstream examples, documentation and development files are omitted. The public parser API does not expose these dependencies’ types.
