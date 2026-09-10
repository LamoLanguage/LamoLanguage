# std.fs

File system operations — wraps C-level fs builtins. All functions are
cross-platform (POSIX + Win32) and operate on UTF-8 paths.

Operations that can fail return `0` on failure and `1` on success
(or `""` for `readText`). This matches the "no exceptions" philosophy
of the language.

## Function Reference

- `exists(p)` — `1` if the path exists, `0` otherwise.
- `isFile(p)` — `1` if `p` is a regular file.
- `isDir(p)` — `1` if `p` is a directory.
- `readText(p)` — return the file contents as a string. Returns `""` on failure.
- `writeText(p, content)` — overwrite `p` with `content`. Returns `1` on success.
- `appendText(p, content)` — append `content` to `p`. Returns `1` on success.
- `delete(p)` — delete a file. Returns `1` on success.
- `createDir(p)` — create a directory. Returns `1` on success.
- `removeDir(p)` — remove an empty directory. Returns `1` on success.
- `copy(src, dst)` — copy a file. Returns `1` on success.
- `move(src, dst)` — move/rename a file. Returns `1` on success.
- `listFiles(p)` — return an array of entry names in directory `p`.
- `size(p)` — file size in bytes (or `0` if not a file).
- `readLines(p)` — read a text file and return an array of its lines (CRLF normalized to LF).
- `writeLines(p, lines)` — write an array of strings to a file, one per line. Returns `1` on success.

## Examples

```lamo
import std.fs as fs

if (fs.exists("config.txt")) {
    let cfg = fs.readText("config.txt")
}

fs.writeText("log.txt", "starting up\n")
fs.appendText("log.txt", "still running\n")

let entries = fs.listFiles(".")
```

## Notes

- `readLines` splits on `"\n"` after normalizing `"\r\n"`. A trailing
  newline produces a trailing empty string element in the result.
- `writeText` truncates any existing file.
