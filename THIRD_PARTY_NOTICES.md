# Third-party components

This library bundles the following source dependencies. Their copyright notices and license texts are retained with the source.

| Component | Version | Purpose | License |
|---|---|---|---|
| [zlib](https://github.com/madler/zlib/tree/v1.3.1) | 1.3.1 | Block decompression | [zlib license](third_party/zlib-1.3.1/LICENSE) |
| [JSON for Modern C++](https://github.com/nlohmann/json/tree/v3.11.3) | 3.11.3 | Reference tables and material assets | [MIT license](third_party/json-LICENSE.MIT) |

Only the zlib source files and build inputs needed by this project are bundled. Upstream examples, documentation and development files are omitted. The public parser API does not expose either dependency's types.
