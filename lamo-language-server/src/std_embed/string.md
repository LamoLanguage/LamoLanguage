# std.string

UTF-8 string utilities. Wraps the C-level string builtins in a
consistent API. Operations are byte-oriented for ASCII characters;
multi-byte UTF-8 sequences are preserved as opaque byte ranges.

## Function Reference

- `length(s)` — number of bytes in `s`.
- `upper(s)` — uppercase ASCII letters.
- `lower(s)` — lowercase ASCII letters.
- `trim(s)` — strip leading and trailing whitespace.
- `startsWith(s, prefix)` — `1` if `s` begins with `prefix`.
- `endsWith(s, suffix)` — `1` if `s` ends with `suffix`.
- `contains(s, needle)` — `1` if `needle` appears in `s`.
- `indexOf(s, needle)` — byte offset of first occurrence, or `-1`.
- `substring(s, start, end)` — substring from `start` (inclusive) to `end` (exclusive); `-1` end means to end of string.
- `replace(s, from, to)` — replace all occurrences of `from` with `to`.
- `split(s, sep)` — split `s` on `sep`, returning an array.
- `charAt(s, idx)` — single-character string at byte `idx`.
- `repeat(s, n)` — `s` concatenated `n` times.
- `slice(s, start)` — substring from `start` to end of string.
- `equals(a, b)` — string equality, returns `1`/`0`.
- `isEmpty(s)` — `1` if `s` is empty.
- `count(s, needle)` — count non-overlapping occurrences of `needle` in `s`.

## Examples

```lamo
import std.string as text

text.length("hello")            // 5
text.upper("hi")                // "HI"
text.trim("  x  ")              // "x"
text.split("a,b,c", ",")        // ["a", "b", "c"]
text.replace("hello", "l", "L") // "heLLo"
text.slice("hello", 2)          // "llo"
text.count("ababab", "ab")      // 3
```

## Notes

- `length` returns byte count, not character count. For ASCII this is
  identical; for UTF-8 with multibyte sequences the two differ.
- `substring` with `end = -1` means "to end of string" (used internally
  by `slice`).
