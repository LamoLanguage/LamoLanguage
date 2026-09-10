# std.path

Cross-platform path manipulation. Provides joining, splitting, and
normalization without touching the filesystem. All functions work on
both forward-slash (POSIX) and backslash (Windows) paths.

## Function Reference

- `join(a, b)` — join two path components with the platform separator.
- `parent(p)` — parent directory of `p` (POSIX `dirname`).
- `filename(p)` — final component of `p` (POSIX `basename`).
- `extension(p)` — file extension including the dot (e.g. `".txt"`), or `""`.
- `absolute(p)` — resolve `p` to an absolute path.
- `normalize(p)` — collapse `.` and `..` segments.
- `basename(p)` — alias for `filename(p)`.
- `dirname(p)` — alias for `parent(p)`.
- `stem(p)` — filename without its extension (`"z.txt"` → `"z"`).
- `hasExtension(p)` — `1` if the path has a non-empty extension.
- `separator()` — OS-native path separator (`"/"` or `"\\"`).

## Examples

```lamo
import std.path as path

path.join("a", "b/c")          // "a/b/c"
path.parent("/x/y/z.txt")      // "/x/y"
path.filename("/x/y/z.txt")    // "z.txt"
path.extension("z.txt")        // ".txt"
path.stem("/x/y/z.txt")        // "z"
path.normalize("/a/b/../c/./d") // "/a/c/d"
```

## Notes

- These functions are purely lexical — they never touch the disk, so
  symlinks and case sensitivity are not considered.
- `separator()` is detected at runtime via `os.name()`.
