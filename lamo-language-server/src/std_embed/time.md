# std.time

Time functions: wall-clock time, monotonic time for elapsed
measurements, and sleep. All times are in milliseconds unless noted
otherwise.

## Function Reference

- `now()` — current wall-clock time in milliseconds since the Unix epoch.
- `timestamp()` — current Unix timestamp in **seconds**.
- `sleep(ms)` — block the current process for `ms` milliseconds. Returns `0`.
- `monotonic()` — current value of the monotonic clock, in milliseconds.
- `elapsed()` — alias for `monotonic()`.

## Examples

```lamo
import std.time as time

let start = time.now()
// ... do work ...
let elapsedMs = time.now() - start

let m = time.monotonic()
time.sleep(100)
let elapsed = time.monotonic() - m   // >= 100
```

## Notes

- Use `monotonic()` (not `now()`) for measuring elapsed time — it is
  not affected by system clock changes.
- The language does not (yet) support first-class function values, so
  there is no `measure(fn)` helper. Use the `let t = time.monotonic();
  ...; time.monotonic() - t` pattern manually.
