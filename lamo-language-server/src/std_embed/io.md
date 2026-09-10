# std.io

Console input/output utilities — wraps the language's core I/O builtins
and adds line-based reading helpers.

## Function Reference

- `print(value)` — print `value` to stdout followed by a newline. Returns `0`.
- `println(value)` — alias for `print`.
- `write(value)` — print `value` to stdout without a trailing newline.
- `eprint(value)` — print `value` to stderr followed by a newline.
- `readLine()` — read one line from stdin (without the trailing newline). Returns `""` on EOF.
- `input(prompt)` — print `prompt` (no newline) then read one line from stdin. Returns the line as a string.
- `inputInt(prompt)` — print `prompt` then read an integer from stdin. Returns `0` if input is not a valid integer.
- `inputStr(prompt)` — alias for `input(prompt)`.

## Examples

```lamo
import std.io as io

io.println("Hello, world!")
io.write("no newline here")
io.eprint("warning on stderr")

// Prompted input
let name = io.input("What is your name? ")
io.println("Hi, " + name)
```

## Notes

- All output is UTF-8 on stdout/stderr.
- `print` and `println` are identical; pick whichever reads best.
- `input(prompt)` always returns a string. Use `inputInt(prompt)` when you
  need an integer (it returns `0` on parse failure rather than aborting).
