# Metadata Property Attributes

Types 84 (`LcOaPropertyAttribute`) and 86 (`LcOaExPropertyAttribute`) store
their property list as ordered name/value pairs. Names remain graph object
references and values retain their native typed `Value` representation.

For metadata stream versions 22 and later, the common object prefix contains
the name and class reference, followed by flags and the property count. The
older native readers add a string to type 84 before the property list. Type 86
inherits that layout and adds a second string after the list. The parser keeps
the first legacy string in `Object::name` and preserves type 86's trailing
legacy string as `Object::strings[0]`. The latter's exact application-level
meaning is not established, so it is exposed as raw data.

The field order is independently confirmed in Freedom 2013 and Manage 2026
`lcodapi.dll` implementations. Freedom 2013 has
`LcOaPropertyAttribute::ReadContents` at RVA `0x1f700` and `WriteContents` at
`0x1f640`; `LcOaExPropertyAttribute::ReadContents` at RVA `0x20130` and
`WriteContents` at `0x20100`. Manage 2026 has the corresponding methods at
`0x7dc90`, `0x7dbe0`, `0x7e8a0` and `0x7e860`. Both generations gate the
additional strings on stream version `< 22`. DLL SHA-256 values are
`1b7600b72594c823dadbb43c3ed0f7aa30d03170625ff863e3f3e23bda9f7ad3`
(Freedom 2013) and
`24af99b9749e971b43b32d0f9c6364783e746e7e22a15c6fffc1e38c775008f3`
(Manage 2026).

This establishes matching field order in two native generations, not universal
cross-exporter equivalence. No pre-22 real sample has yet confirmed the
parser's string encoding or the trailing type 86 string's semantics.
