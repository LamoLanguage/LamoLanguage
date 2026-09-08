# std.collections

Higher-level collection types built on top of Lamo's native arrays:
**List**, **Stack**, **Queue**, **HashMap**, and **HashSet** — plus typed
variants of each and the `Option`/`Result` error-handling API (2.5.0).

Because Lamo's static type inference cannot follow struct types across
module boundaries, the API uses module-level functions whose first argument
is the collection (similar to Python's `heapq` module).

Internally, all collections are represented as plain arrays:

| Type | Layout |
|------|--------|
| List | `[v0, v1, ...]` |
| Stack | `[bottom, ..., top]` (push/pop from the end) |
| Queue | `[front, ..., back]` (enqueue at end, dequeue at front) |
| HashMap | `[k0, v0, k1, v1, ...]` (flat key/value pairs) |
| HashSet | `[v0, v1, ...]` (no duplicates) |

## Function Reference

### List

- `newList()` — create an empty list.
- `listPush(list, value)` — append; returns new length.
- `listPop(list)` — remove and return the last element.
- `listGet(list, i)` / `listSet(list, i, value)` — index access.
- `listInsert(list, i, value)` — insert at index.
- `listRemoveAt(list, i)` — remove and return element at index.
- `listContains(list, value)` — `1` if present.
- `listIndexOf(list, value)` — index of first match, or `-1`.
- `listLen(list)` / `listIsEmpty(list)` / `listClear(list)`.

### Stack (LIFO)

- `newStack()` — create an empty stack.
- `stackPush(stack, value)` — push; returns new length.
- `stackPop(stack)` — pop the top.
- `stackPeek(stack)` — return the top without removing (returns `0` if empty).
- `stackLen` / `stackIsEmpty` / `stackClear`.

### Queue (FIFO)

- `newQueue()` — create an empty queue.
- `queueEnqueue(queue, value)` — append; returns new length.
- `queueDequeue(queue)` — remove and return the front (returns `0` if empty).
- `queuePeek(queue)` — return the front without removing.
- `queueLen` / `queueIsEmpty` / `queueClear`.

### HashMap

- `newHashMap()` — create an empty map.
- `mapPut(map, key, value)` — set `key` to `value`; returns `value`.
- `mapGet(map, key)` — return the value for `key` (returns `0` if missing).
- `mapGetOr(map, key, default)` — return the value or `default`.
- `mapHas(map, key)` — `1` if present.
- `mapRemove(map, key)` — `1` if removed, `0` if not found.
- `mapLen(map)` / `mapKeys(map)` / `mapValues(map)` / `mapClear(map)`.

### HashSet

- `newHashSet()` — create an empty set.
- `setAdd(set, value)` — add if not present; returns new length.
- `setHas(set, value)` / `setContains(set, value)` — `1` if present.
- `setRemove(set, value)` — `1` if removed.
- `setLen` / `setIsEmpty` / `setClear` / `setToArray`.

## Typed API (2.5.0)

The functions above are untyped: nothing stops you from mixing `int`s and
`string`s in the same list. The **typed** variants wrap the same erased
representations with generic signatures, so mixing element types is a
compile-time error instead of silent data corruption. Runtime behavior and
cost are identical (type erasure — zero boxing).

### TypedList<T>

- `typedListNew<T>() -> array<T>` — create an empty typed list.
- `typedListPush<T>(xs: array<T>, value: T) -> int`
- `typedListPop<T>(xs: array<T>) -> T`
- `typedListGet<T>(xs: array<T>, i: int) -> T`
- `typedListLen<T>(xs: array<T>) -> int`

### TypedMap<K, V>

- `typedMapNew<K, V>()` — create an empty typed map.
- `typedMapHas<K, V>(m, key: K) -> bool`
- `typedMapGetOr<K, V>(m, key: K, fallback: V) -> V`
- `typedMapLen<K, V>(m) -> int`

### TypedSet<T>

- `typedSetNew<T>()` — create an empty typed set.
- `typedSetAdd<T: Eq>(s: array<T>, value: T) -> bool`
- `typedSetHas<T: Eq>(s: array<T>, value: T) -> bool`
- `typedSetLen<T>(s: array<T>) -> int`

## Option<T> / Result<T, E> (2.5.0)

Sentinel values (`0`, `""`, `-1`) are easy to misuse. The `Option` and
`Result` APIs provide type-checked factories and accessors instead. The
generic struct definitions also parse and check, but payloads crossing
module boundaries await the module-type-flow fix — until then, use the
function-shaped API below (see `docs/RFC-generics.md` §10).

**Representation** (erased arrays, same as everything else in this module):

- `Option<T>` — `[present: int, value]`
- `Result<T, E>` — `[ok: int, value, error]`

### Option API

- `optionSome<T>(value: T)` — wrap a value.
- `optionNone<T>()` — an absent value.
- `optionIsSome<T>(opt) -> bool` — `1` if present.
- `optionUnwrapOr<T>(opt, fallback: T) -> T` — the value, or `fallback`.

### Result API

- `resultOk<T, E>(value: T)` — wrap a success value.
- `resultErr<T, E>(error: E)` — wrap an error.
- `resultIsOk<T, E>(res) -> bool` — `1` if success.
- `resultUnwrapOr<T, E>(res, fallback: T) -> T` — the value, or `fallback`.

```lamo
import std.collections as col

fn findUser(id: int) -> array {
    if (id != 7) { return col.optionNone() }
    return col.optionSome("Arthur")
}

let name = col.optionUnwrapOr(findUser(7), "anon")
print(name)   // "Arthur"
```

## Examples

```lamo
import std.collections as collections

let list = collections.newList()
collections.listPush(list, 10)
collections.listPush(list, 20)
collections.listGet(list, 0)   // 10

let stack = collections.newStack()
collections.stackPush(stack, "a")
collections.stackPush(stack, "b")
collections.stackPop(stack)    // "b"

let map = collections.newHashMap()
collections.mapPut(map, "name", "lamo")
collections.mapGet(map, "name")  // "lamo"

let set = collections.newHashSet()
collections.setAdd(set, "apple")
collections.setHas(set, "apple")  // 1
```

## Notes

- HashMap keys may be any value comparable with `==` (strings, ints,
  floats). The map performs a linear scan, so it is best suited to small
  key sets.
- `queueDequeue` and `queuePeek` return `0` (not an error) on an empty
  queue — callers should check `queueIsEmpty` first if `0` is a valid
  element.
- Prefer the typed variants for new code: they cost nothing at runtime and
  catch mixing bugs at compile time.
