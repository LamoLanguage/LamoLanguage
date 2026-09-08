# std.env

Environment variable access. Wraps the C-level env builtins
(`getenv`, `setenv`, `unsetenv`). Cross-platform (Win32 + POSIX).

## Function Reference

- `get(name)` — return the value of variable `name`, or `""` if unset.
- `set(name, value)` — set `name` to `value`. Returns `0` on success.
- `remove(name)` — unset `name`. Returns `0` on success.
- `has(name)` — `1` if `name` is set to a non-empty value, `0` otherwise.
- `getOr(name, default)` — return `name`'s value, or `default` if unset.

## Examples

```lamo
import std.env as env

let path = env.get("PATH")
env.set("LAMO_VERBOSE", "1")

if (env.has("LAMO_VERBOSE")) {
    print("verbose mode on")
}

let mode = env.getOr("LAMO_MODE", "release")
env.remove("LAMO_VERBOSE")
```

## Notes

- `has` returns `0` for variables that are set but empty. If you need
  to distinguish "set but empty" from "unset", check `get` directly
  against `""` (both cases look the same in this API).
- Variables set with `set` are visible to subsequent child processes
  spawned via `std.process.run`/`exec`.
