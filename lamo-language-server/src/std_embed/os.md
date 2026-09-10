# std.os

Operating system information. Read-only access to host OS metadata:
name, architecture, CPU count, home directory, temp directory.

## Function Reference

- `name()` — `"linux"`, `"macos"`, `"windows"`, `"freebsd"`, or `"openbsd"`.
- `arch()` — CPU architecture (`"x64"`, `"arm64"`, etc.).
- `cpuCount()` — number of logical CPUs.
- `home()` — current user's home directory.
- `tempDir()` — system temp directory.
- `isWindows()` — `1` if running on Windows, `0` otherwise.
- `isPosix()` — `1` on linux/macos/freebsd/openbsd, `0` otherwise.
- `lineEnding()` — OS-native line ending (`"\r\n"` on Windows, `"\n"` on POSIX).

## Examples

```lamo
import std.os as os

print(os.name())        // "linux"
print(os.arch())        // "x64"
print(os.cpuCount())    // 8
print(os.home())        // "/home/user"
print(os.tempDir())     // "/tmp"

if (os.isWindows()) {
    // ... Windows-specific code ...
}
```

## Notes

- `name()` returns lowercase identifiers; compare with the literals
  listed above rather than `"Linux"` or `"Windows"`.
- All functions are read-only and cheap to call — they cache nothing,
  so they're safe to call in tight loops.
