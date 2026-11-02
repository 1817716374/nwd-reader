# Metadata Property Attributes

Types 84 (`LcOaPropertyAttribute`) and 86 (`LcOaExPropertyAttribute`) store their property list as ordered name/value pairs. Names remain graph object references and values retain their native typed `Value` representation.

For metadata stream versions 22 and later, the common object prefix contains the name and class reference, followed by flags and the property count. The older native readers add a string to type 84 before the property list. Type 86 inherits that layout and adds a second string after the list. The parser keeps the first legacy string in `Object::name` and preserves type 86's trailing legacy string as `Object::strings[0]`. The latter's exact application-level meaning is not established, so it is exposed as raw data.

The layout distinction is based on Manage 2026 `lcodapi.dll` read/write implementations: `LcOaPropertyAttribute::ReadContents` at RVA `0x7dc90` and `WriteContents` at `0x7dbe0`; `LcOaExPropertyAttribute::ReadContents` at RVA `0x7e8a0` and `WriteContents` at `0x7e860`. Both derived readers gate the additional strings on stream version `< 22`. DLL SHA-256: `24af99b9749e971b43b32d0f9c6364783e746e7e22a15c6fffc1e38c775008f3`.

This establishes the version-gated field order in that native implementation, not cross-version or cross-exporter equivalence. No pre-22 real sample has yet confirmed the parser's string encoding or the trailing type 86 string's semantics.
