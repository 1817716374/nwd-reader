# Metadata Light Types

## Confirmed class identities

Freedom 2013's native type-name pointer table directly maps index 27 to
`LcOaDistantLight`, 28 to `LcOaPointLight`, and 29 to `LcOaSpotLight`. Manage
2017 and Manage 2026 registrations independently preserve these IDs. RTTI in
the newer generations places point and spot lights below `LcOaPositionLight`,
`LcOaCommonLight`, and `LcOaLight`.

The public `metadata_type_name(type)` helper returns these static names without
allocation. An empty result means that this library has not yet verified a
registered name for that numeric type; it does not mean the object is
unsupported.

## Payload status

Type 27 contains 12 numeric values in the 431 and 448 sample families. The
Freedom 2013 and Manage 2026 native `LcOaDistantLight::ReadContents` and
`WriteContents` call the common-light serializer, then read/write a three-value
direction array. The native getters identify that array as the direction; the
common-light getters identify the preceding nine values as the three colors.
`metadata_distant_light_fields()` exposes these names and preserves the raw
order in `Object::numbers`.

| `Object::numbers` | Type 27: DistantLight |
| --- | --- |
| 0–2 | Ambient color |
| 3–5 | Diffuse color |
| 6–8 | Specular color |
| 9–11 | Direction |

The sample census found type 27 in container versions 72, 431, and 448. The
semantic helper rejects other numeric counts. Units, color space, and vector
coordinate frame remain unresolved.

The type-27 wire layout also exposes light state: `Object::integers[0]` retains
the raw `on` value and `Object::flags` retains the following state word (with a
version-scoped reference between them when present). Native
`LcOaCommonLight::WriteContents` packs `eye` into bit 0 and `cast shadow` into
bit 1 for the supported stream versions; unknown bits remain available in the
raw word and in `MetadataLightStateFields::unknown_state_bits`.
`metadata_light_state_fields()` returns these values without changing the
stored object. It also accepts a type-27 object in `SavedItems.objects`;
`SavedLight::object` indexes that local graph, so callers should not compare it
directly with a `Model::graphs` object ID. The 48-file 2014–2023 sample scan
found 190 model-graph type-27 objects and 102 saved-light type-27 objects; all
had `on=1` and state word 0. The scan found no type-28/29 records in these
modern files. Autodesk's `Brewery.nwd` training sample adds two type-27
objects at container version 72; both also have `on=1` and state word 0.
Nonzero native state bits have not yet been observed in a real sample.

The common-light wire prefix contains the raw `on` value and the eye/shadow
state word. Container version 103 uses the version-5-and-later numeric layouts:
16 values for type 28 and 21 for type 29. Freedom 2013's native
`ReadContents`/`WriteContents` and getters map the numeric values as follows:

| Object::numbers | Type 28: PointLight | Type 29: SpotLight |
| --- | --- | --- |
| 0–2 | Ambient color | Ambient color |
| 3–5 | Diffuse color | Diffuse color |
| 6–8 | Specular color | Specular color |
| 9–11 | Position | Position |
| 12–14 | Attenuation | Attenuation |
| 15 | Radius of influence | Radius of influence |
| 16–18 | — | Direction |
| 19 | — | Drop-off rate |
| 20 | — | Cutoff angle |

The pre-version-5 serializers have 15 point-light and 20 spot-light values;
their spot order places direction before attenuation and has no radius.
`metadata_light_fields()` supports both known numeric orders. The version-103
metadata reader consumes the version-5-and-later 16/21-value layouts, while
`Object::numbers` preserves the original values and order.

Point/spot and distant-light records share the native state mapping:
`Object::integers[0]` is the raw `on` value; `Object::flags` is the eye/shadow
word, with bit 0 for eye and bit 1 for cast shadow. Unknown state bits are
preserved. The version-103 sample confirms the field order and includes both
on and off point lights, but its observed state words are zero. Nonzero
point/spot state values still lack real-sample confirmation. Units, color
space, coordinate frame, and scalar ranges are not established. Type 28/29
numeric layouts are still accepted only for container version 103.

Evidence was obtained from the native serializers and the Freedom 2013 version-103 sample. Physical units, color space, and coordinate conventions remain unverified.
