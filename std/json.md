# std.json

JSON parsing and serialization. A pure-Lamo implementation.

## Representation

JSON values are mapped to Lamo values as follows:

| JSON       | Lamo                                   |
|------------|----------------------------------------|
| object     | 2-element array: `[keys, values]`      |
| array      | Lamo array                             |
| string     | Lamo string                            |
| integer    | Lamo int                               |
| float      | Lamo string (Lamo can't parse floats yet) |
| `true`     | int `1`                                |
| `false`    | int `0`                                |
| `null`     | int `-1` (sentinel)                    |

## Function Reference

- `parse(text)` — parse a JSON string into Lamo values.
- `stringify(value)` — serialize a Lamo value back to a JSON string.
- `get(obj, key)` — value at `key`, or `-1` (null sentinel) if missing.
- `getString(obj, key)` — alias for `get` (string value).
- `getInt(obj, key)` — alias for `get` (int value).
- `has(obj, key)` — `1` if `key` is present in the object.
- `keys(obj)` — array of the object's keys.
- `values(obj)` — array of the object's values.
- `len(obj)` — number of keys in the object.

## Examples

```lamo
import std.json as json

let v = json.parse("{\"name\":\"lamo\",\"version\":1}")
json.getString(v, "name")     // "lamo"
json.getInt(v, "version")     // 1
json.has(v, "version")        // 1
json.len(v)                   // 2

json.stringify("hello")       // "\"hello\""
json.stringify(42)            // "42"
json.stringify([1, 2, 3])     // "[1,2,3]"

let arr = json.parse("[1, 2, 3, 4, 5]")
arr[0]                        // 1
```

## Notes

- JSON objects are represented as `[keys_array, values_array]` rather
  than as a struct, because Lamo's static type inference cannot follow
  struct types across module boundaries. Use the `json.get` /
  `json.getString` / `json.getInt` helpers rather than indexing into
  the array directly.
- `stringify` recognizes the object representation by structural shape
  (a 2-element array of two arrays). A regular 2-element array of
  arrays would be stringified as an object — keep this in mind.
- Floats round-trip as strings: parsing `3.14` yields the string
  `"3.14"`, not a Lamo float.
