# std.process

Process operations: access to the current process ID, running shell
commands, capturing their output, and exiting the program.

## Function Reference

- `currentPid()` — OS process ID of the current process.
- `pid()` — short alias for `currentPid()`.
- `run(cmd)` — execute a shell command synchronously. Returns the exit code.
- `exec(cmd)` — execute a shell command synchronously and return its stdout as a string (stderr is not captured). Returns `""` if the command cannot be started.
- `exit(code)` — terminate the program with exit code `code`.

## Examples

```lamo
import std.process as process

print(process.currentPid())
let exitCode = process.run("echo hello")
let output = process.exec("whoami")
print(output)

// Terminate early on error
if (exitCode != 0) {
    process.exit(1)
}
```

## Notes

- `run` and `exec` invoke the shell (`/bin/sh -c` on POSIX, `cmd /c`
  on Windows). Never pass unsanitized user input as the command string.
- `exec` preserves the command's trailing newline (e.g. `exec("echo hi")`
  returns `"hi\n"`, not `"hi"`). Strip it with `string.trim` if needed.
