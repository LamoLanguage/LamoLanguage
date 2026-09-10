# std.testing

Official testing framework. A tiny but complete test runner with
assertions.

Because Lamo does not (yet) support first-class function values or
anonymous functions, the API uses a **begin/end** pattern: call
`begin(name)` to start a test case, run your assertions, then call
`end()` to finish. At the end of the file, call `summary()` to print
the result.

## Function Reference

- `begin(name)` — start a named test case.
- `end()` — finish the current test case; increments pass/fail counters.
- `assert(condition)` — pass silently if truthy; record a failure otherwise.
- `assertEqual(actual, expected)` — pass if `actual == expected`.
- `assertTrue(value)` — pass if `value` is truthy.
- `assertFalse(value)` — pass if `value` is falsy.
- `fail(message)` — always record a failure.
- `summary()` — print the pass/fail summary. Returns `0` if all passed, `1` otherwise.
- `reset()` — clear all counters (useful in long-running REPL sessions).

## Examples

```lamo
import std.testing as testing

fn add(a, b) { return a + b }

testing.begin("add basic")
testing.assertEqual(add(2, 3), 5)
testing.end()

testing.begin("add negative")
testing.assertEqual(add(-1, 1), 0)
testing.end()

testing.begin("truthy check")
testing.assertTrue(1)
testing.end()

testing.summary()
```

## Notes

- Failures are accumulated within a test case; the case still runs to
  `end()`. A case with any failed assertion is counted as failed.
- Pass messages go to stdout; failure messages go to stderr, so
  pipelines that capture stdout see only the pass lines.
- `assertEqual`'s failure message interpolates `actual` and `expected`
  via string concatenation, so prefer comparing values of the same
  type (int to int, string to string).
