# std.debug

Development and debugging utilities. Lightweight tools for ad-hoc
debugging: `log`/`dump` for inspecting values, `time`/`timeEnd` for
simple profiling, and `trace`/`assert` for diagnostics.

All output goes to **stderr** so it doesn't interfere with stdout-based
pipelines.

## Function Reference

- `log(message)` — print a debug message to stderr with a `debug: ` prefix. Returns `0`.
- `dump(value)` — print `value` to stderr for inspection. Returns `0`.
- `trace(message)` — print a stack-trace-style line (`trace: <message>`). Returns `0`.
- `time(label)` — start a named timer.
- `timeEnd(label)` — stop a named timer, print elapsed ms, and return the elapsed value. Prints an error if `label` is not active.
- `assert(condition, message)` — if `condition` is falsy, print `message` to stderr and exit the program with code `1`.

## Examples

```lamo
import std.debug as debug

debug.log("reached point A")
debug.dump(myValue)

debug.time("myloop")
// ... do work ...
debug.timeEnd("myloop")   // prints "myloop: 12ms" to stderr

debug.trace("reached end of program")

// Invariant check
debug.assert(x >= 0, "x must be non-negative")
```

## Notes

- `dump` converts its argument to a string with `+ ""`. For complex
  values (arrays, structs) the output may be terse — wrap it in
  `json.stringify` for a more readable view.
- Timers are stored in module-level parallel arrays. Calling `time`
  with the same label twice pushes a second entry; `timeEnd` removes
  the most recent matching entry.
- `assert` calls `process.exit(1)` on failure, so it cannot be used
  inside `std.testing` test cases (which expect cases to run to
  completion). Use `testing.assert` / `testing.fail` inside tests.
