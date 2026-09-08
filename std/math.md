# std.math

Math functions and constants. Wraps the C-level math builtins and
exposes a small set of mathematical constants. Trigonometric functions
work in radians.

## Constants

- `PI`  — π ≈ 3.141592653589793
- `E`   — Euler's number ≈ 2.718281828459045
- `TAU` — 2π ≈ 6.283185307179586

## Function Reference

- `sqrt(x)` — square root.
- `pow(base, exp)` — `base` raised to `exp`.
- `sin(x)` / `cos(x)` / `tan(x)` — trigonometric functions (radians).
- `floor(x)` / `ceil(x)` / `round(x)` — rounding.
- `min(a, b)` / `max(a, b)` — comparisons.
- `clamp(v, lo, hi)` — clamp `v` to `[lo, hi]`.
- `abs(x)` — absolute value.
- `sum(arr)` — sum of numeric elements.
- `avg(arr)` — arithmetic mean (returns `0` for empty arrays).
- `gcd(a, b)` — greatest common divisor (Euclidean).
- `factorial(n)` — `n!` for non-negative `n` (returns `0` if `n < 0`).
- `toRadians(deg)` — convert degrees to radians.
- `toDegrees(rad)` — convert radians to degrees.

## Examples

```lamo
import std.math as math

math.sqrt(16)            // 4
math.pow(2, 10)          // 1024
math.sin(math.PI / 2)    // 1
math.clamp(15, 0, 10)    // 10
math.gcd(12, 18)         // 6
math.factorial(5)        // 120
math.sum([1, 2, 3, 4, 5])  // 15
```

## Notes

- `sqrt`, `floor`, `ceil`, `round`, `min`, `max`, `clamp`, `pow`
  delegate to C runtime functions; their result type follows the C
  convention (e.g. `sqrt(16)` returns the integer `4`, not `4.0`).
- `abs` is implemented in pure Lamo to avoid recursion with the
  language-level builtin of the same name.
