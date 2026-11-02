# Publish Attribute Fields

`metadata_publish_fields(object, container_version)` returns named fields for
type 92 in container version 103. It returns no value for other versions,
object types, or field counts.

The string fields are IDs into the owning `ObjectGraph::strings` array. Keep
the graph context when resolving them:

```cpp
if (const auto fields = nwd::metadata_publish_fields(object, document.version())) {
  const auto title_id = fields->title;
  if (title_id != nwd::none && title_id < graph.strings.size()) {
    const auto &title = graph.strings[title_id];
  }
}
```

| Named field | Source |
| --- | --- |
| `title`, `author`, `subject`, `publisher` | `Object::strings[0..3]` |
| `copyright`, `published_for`, `comments`, `keywords` | `Object::strings[4..7]` |
| `publish_date`, `expiry_date` | `Object::wide[0..1]` |
| `flags` | `Object::integers[0]` |

Date values are raw 64-bit integers; their epoch and unit are not identified.
The `has_expiry_date` flag reports whether the expiration date is enabled.
Unrecognized bits remain available in `flags` and `unknown_flags`.

| Bit | Field |
| ---: | --- |
| 0 | `has_password` |
| 1 | `display_on_open` |
| 4 | `has_expiry_date` |
| 5 | `has_been_resaved` |
| 6 | `allow_resave` |
| 7 | `display_at_password` |
| 8 | `embed_textures` |
| 9 | `embed_database_properties` |
| 10 | `prevent_object_property_export` |

The serialized record contains the password-presence flag but not the in-memory
password string. The API does not expose a password value.
