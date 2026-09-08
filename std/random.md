# std.random

Pseudo-random number generation. Uses an xorshift64* generator.
**Not cryptographically secure** — use it for games, simulations, and
testing only.

The generator is seeded automatically on first use with time + pid,
but you can re-seed deterministically with `seed()`.

## Function Reference

- `seed(s)` — re-seed the generator with integer `s`.
- `int(lo, hi)` — random integer in `[lo, hi]` (inclusive on both ends).
- `range(lo, hi)` — random integer in `[lo, hi)` (excludes `hi`).
- `float()` — random float in `[0.0, 1.0)`.
- `floatRange(lo, hi)` — random float in `[lo, hi)`.
- `bool()` — random boolean (`0` or `1`).
- `choice(arr)` — random element from a non-empty array (aborts if empty).
- `shuffle(arr)` — Fisher-Yates shuffle of `arr` in place. Returns the same array.
- `sample(arr, k)` — new array of `k` distinct elements from `arr` (without replacement).

## Examples

```lamo
import std.random as random

random.seed(42)
let x = random.int(1, 100)      // 1..100 inclusive
let f = random.float()          // [0.0, 1.0)
let b = random.bool()           // 0 or 1
let pick = random.choice([1, 2, 3])

let deck = [1, 2, 3, 4, 5]
random.shuffle(deck)

let hand = random.sample(deck, 2)
```

## Notes

- For deterministic test output, call `seed()` once before generating
  any numbers.
- `shuffle` mutates the input array in place and also returns it; the
  return value is provided for chaining only.
- `sample` returns a shuffled copy when `k >= arr.len()`.
