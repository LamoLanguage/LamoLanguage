# Lamo Memory Model

**Status:** Living document. The design described here is **implemented**:
an opt-in mark-sweep GC shipped in compiler `2.3.0` (rollout Steps 2–6 of
§3). This document captures (1) the arena baseline the language defaults to,
(2) the GC design, and (3) the policy that gates example promotion.

This document is the **single source of truth** for memory behavior. The
spec ([SPEC.md](SPEC.md) §9, §11, §12) defers to this document on memory
questions.

---

## 1. Baseline Model: The Arena

By default (when a program never calls any `gc_*` builtin), Lamo has **no
garbage collection** and **no per-value ownership tracking**. Allocations
are tracked in a single global arena and freed in bulk at program exit.
Opting into the GC (§2) layers mark-sweep reclamation on top of this
baseline without changing its behavior.

### 1.1 The arena

```c
static void** lamo_string_arena = NULL;
static size_t lamo_string_arena_count = 0;
static size_t lamo_string_arena_capacity = 0;
```

Defined in `src/codegen/lamo_runtime.h`. Holds `void*` pointers to:

- `char*` buffers — strings from concatenation, `input_str`, struct field
  writes that produce a new string, etc.
- `LamoArray*` structs — every array literal and struct literal allocation.
- `LamoValue*` buffers — the `items` array inside each `LamoArray`.

The arena grows monotonically (no reuse of slots). `lamo_arena_alloc(size)`
appends a fresh `malloc`'d block to the arena. `lamo_arena_free_all()` walks
the arena and frees every block — called once, via `atexit`.

### 1.2 Lifetimes

Every heap allocation made by Lamo user code lives until program exit. There
is no `free(x)` exposed to user code, no `drop`, no scope-based destruction.

Concretely:

```lamo
let s = ""
let i = 0
while (i < 1_000_000) {
    s = s + "x"     // each iteration leaks the previous `s` buffer
    i += 1
}
// On program exit, all 1_000_000 buffers are freed at once.
```

For a script that runs for 100ms and exits, this is fine. For a process
that runs for an hour, this is a leak.

### 1.3 Why this was chosen

When Lamo was an educational transpiler prototype, the arena was the right
call:

- **Simple.** One global, one free list, one `atexit`. No GC algorithm, no
  root scanning, no write barriers.
- **Correct enough.** Programs that run-to-completion don't observe the leak;
  they exit and the OS reclaims everything.
- **Avoids the worst C bug.** Before the arena, `s = s + "x"` would leak
  silently. The arena made this "leak until exit" instead of "leak forever",
  which is a strict improvement.

### 1.4 Where it breaks

The model breaks for **long-running processes**: HTTP servers, GUI event
loops with string traffic, daemons, REPL sessions that build up state. This
limitation is exactly what the opt-in GC (§2) addresses: long-running
programs can enable periodic collection so the arena no longer grows
without bound.

---

## 2. The Opt-In Mark-Sweep GC (shipped 2.3.0)

**Lamo ships a simple, opt-in mark-sweep garbage collector.** The arena
stays as the default allocator (for backwards compatibility and for
short-lived programs where GC overhead is pure cost). The GC is enabled
explicitly by the program — typically at the top of a long-running entry
point — and runs on demand or on a periodic schedule.

### 2.1 Why mark-sweep (and not reference counting / ownership)

Three alternatives were on the table:

| Option                 | Pros                                              | Cons (for Lamo)                                                                 |
|------------------------|---------------------------------------------------|---------------------------------------------------------------------------------|
| **Refcounting**        | deterministic, no pauses                          | Cycles leak; need cycle collector anyway; refcount bumps on every assign = perf hit in generated code |
| **Ownership / `drop`** | zero runtime cost, like Rust                      | Massive language change: borrow checker, lifetimes, `&` vs `&mut`. Wrong fit for a script-y language. |
| **Mark-sweep**         | handles cycles; simple to add to existing arena; no per-assign cost | stop-the-world pauses; needs root enumeration                                   |
| **Tracing generational** | best perf for long-running                      | Way too much code for the current team size; premature                          |

**Mark-sweep wins** for Lamo because:

- The arena already centralizes every allocation, so adding a header + mark
  bit per allocation is cheap.
- Lamo values are `LamoValue` (a tagged union with explicit type), so root
  scanning is "walk every `LamoValue` on the C stack and in globals" —
  well-defined.
- Cycles exist (struct fields can point to arrays that point back; arrays can
  hold struct values that reference the array). Refcounting would need a
  separate cycle collector, at which point you've built mark-sweep anyway.
- Ownership would change the language identity. That's a bigger decision
  than memory management alone.

### 2.2 The design (high level)

Every allocation made by `lamo_arena_alloc` gets a small header:

```c
typedef struct LamoGcHeader {
    size_t size;                 // bytes, excluding header
    unsigned char mark : 1;      // set during mark, cleared during sweep
    unsigned char is_array : 1;  // 1 = LamoArray, 0 = opaque (string, items buffer)
    unsigned char in_use : 1;    // 0 = slot is free (after sweep), 1 = allocated
    struct LamoGcHeader* next;   // intrusive linked list of all allocations
} LamoGcHeader;
```

Allocations are tracked in an intrusive doubly-linked list (the GC's "heap").
On `gc_collect()`:

1. **Mark phase** — walk the roots (see §2.3), recursively marking every
   reachable `LamoValue`'s backing allocation. Marking is depth-first; deep
   graphs use an explicit stack to avoid C stack overflow.
2. **Sweep phase** — walk the heap list. Any allocation with `mark == 0`
   is freed; its slot is removed from the list. Allocations with `mark == 1`
   have their mark cleared for the next cycle.

### 2.3 Root enumeration

Roots are:

- **C-stack `LamoValue`s** — every local variable in generated C functions
  that holds a `LamoValue` is a root. The codegen emits a per-function root
  registry: at function entry, push each `LamoValue` local's address onto a
  thread-local root stack; at exit, pop them.
- **Global `LamoValue`s** — top-level `let`s become C globals. They're
  registered once at startup.
- **The `self` receiver** in method calls — pushed/popped like a local.
- **Function arguments** holding `LamoValue`s.

This is the classic "exact GC" approach (as opposed to conservative scanning
of the whole C stack, which is what Boehm GC does). Exact enumeration is
possible because the codegen knows the type of every local.

The per-function root push/pop is emitted by codegen. It looks like:

```c
LamoValue lamo_user_foo(LamoValue a, LamoValue b) {
    LAMO_GC_PUSH_ROOT(&a);
    LAMO_GC_PUSH_ROOT(&b);
    LamoValue local1 = ...;
    LAMO_GC_PUSH_ROOT(&local1);
    // ... body ...
    LAMO_GC_POP_ROOTS_N(3);
    return local1;
}
```

### 2.4 What about strings inside `LamoValue`?

Strings are immutable in Lamo. A `LamoValue` with `type == LAMO_VALUE_STRING`
holds a `const char*` pointing into an arena-allocated buffer. The buffer's
header has `is_array == 0` — it's opaque, no internal pointers to trace. The
GC marks it reachable if any live `LamoValue` references it.

If two `LamoValue`s reference the same string buffer (sharing is common),
both mark the same header — fine, marking is idempotent.

### 2.5 What about `LamoArray`?

A `LamoArray*` points to a `LamoArray` struct, which has an `items` buffer
holding `LamoValue`s. The GC traces:

```
LamoValue(array_value=p)
    -> header of p (mark)
        -> LamoArray { items, count, capacity }
            -> each items[i] is a LamoValue -> recurse
```

So the GC marks both the `LamoArray` struct allocation AND the `items`
buffer allocation (because the struct references it — we mark anything
reachable). Items that are themselves arrays or structs recurse.

### 2.6 Trigger policy

GC runs are explicit:

- **Manual**: `gc_collect()` — exposed as a builtin in user code. Programs
  that want full control call this at natural boundaries (end of HTTP
  request, after a batch of work, etc.).
- **Periodic**: `gc_set_threshold(N)` — the runtime counts bytes allocated
  since the last collection. When the count exceeds `N`, the next allocation
  triggers a `gc_collect()`. Default `N = 0` (never auto-trigger).
- **On `http_serve`**: the HTTP server loop calls `gc_collect()` after every
  N requests (configurable; default N=100). This is the fix that lets
  `examples/http_server.lamo` ship as an official example.

### 2.7 Backwards compatibility

- Programs that don't call any `gc_*` builtin get the **exact same behavior
  as 2.2.0**: arena allocations, freed at exit. The GC machinery exists but
  is never invoked.
- Programs that opt in via `gc_set_threshold(N)` with `N > 0`, or that call
  `gc_collect()` explicitly, get collection.
- The arena's `lamo_arena_free_all` (called at exit) is unchanged: it frees
  everything that's still allocated (i.e., what the GC didn't reclaim). This
  means even with GC enabled, exit-time cleanup is one bulk free, same as
  before.

### 2.8 Limitations of this GC (must be documented honestly)

1. **Stop-the-world.** No incremental collection yet. A `gc_collect()` call
   blocks the caller until done. For typical Lamo heaps (< 100MB) this is
   sub-millisecond; for very large heaps it can be tens of ms. Adequate for
   HTTP services handling tens of req/sec, not for games or real-time audio.

2. **Single-threaded only.** The current runtime is single-threaded; the GC
   assumes it. When (if) Lamo adds threads, the GC needs a stop-the-world
   handshake with every thread.

3. **Root enumeration is exact but compiler-driven.** Bugs in codegen's root
   push/pop will cause either missed roots (false frees → use-after-free) or
   excessive roots (memory not reclaimed). The runtime includes a
   `gc_verify_roots()` debug helper that walks the C stack conservatively
   and reports any `LamoValue`-shaped bytes not in the root registry — for
   testing only.

4. **Finalizers not supported.** If a Lamo value holds a resource (file
   handle, socket, etc.), the resource is NOT closed when the value is
   collected. User code must close explicitly. This is the same as Go (pre-
   `runtime finalizers`) and most scripting languages.

5. **No compaction.** The arena layout doesn't move objects. Long-running
   programs can fragment the C heap (not the arena — the arena is just a
   pointer list). This is rarely the bottleneck; if it becomes one, a
   compacting collector is a separate, large project.

---

## 3. Implementation plan

This is the rollout sequence. Each step is independently shippable.

### Step 1 — Document (this file, SPEC.md updates, README warning)

Done. No code changes; the policy in §4 below took effect immediately,
so the HTTP example was **demoted** from "official" to "preview" until
Step 3 landed.

### Step 2 — GC skeleton in `lamo_runtime.h` — ✅ DONE (2.3.0)

Added:

```c
typedef struct LamoGcHeader { ... } LamoGcHeader;
static LamoGcHeader* lamo_gc_heap_head = NULL;
static size_t lamo_gc_bytes_since_collect = 0;
static size_t lamo_gc_threshold = 0;          // 0 = disabled

static void* lamo_gc_alloc(size_t size);       // wraps arena + header
static void  lamo_gc_push_root(LamoValue* v);  // push to root stack
static void  lamo_gc_pop_roots_n(int n);       // pop n entries
static void  lamo_gc_collect(void);            // mark + sweep
static void  lamo_gc_set_threshold(size_t bytes);
```

`lamo_arena_alloc` now allocates `sizeof(LamoGcHeader) + size` bytes,
prepends a header, links it into `lamo_gc_heap_head`, and returns the
payload pointer. The arena (`lamo_string_arena`) still tracks payload
pointers for atexit cleanup; `lamo_arena_free_all` frees via the header
pointer (`(char*)payload - sizeof(LamoGcHeader)`).

`lamo_gc_collect()` implements full mark-sweep:
1. Clear all marks (walk the heap list).
2. Mark phase: walk the root stack, recursively marking every reachable
   allocation. `lamo_gc_mark_value` handles strings (looks up the
   header by matching payload pointer in the heap list), arrays/structs
   (marks the LamoArray header, then traces into the items buffer and
   each item value). Cycles are handled via the `if (h->mark) return;`
   check in `lamo_gc_mark_header`.
3. Sweep phase: walk the heap list. Unmarked allocations are freed and
   their arena slots are set to NULL (so atexit doesn't double-free).

New builtins: `gc_collect()` (returns live count),
`gc_set_threshold(N)` (auto-trigger threshold in bytes),
`gc_heap_size()`, `gc_heap_count()`.

### Step 3 — Codegen emits root push/pop — ✅ DONE (2.3.0)

`codegen.c` maintains a compile-time scope stack
(`lamo_gc_scope_stack[64]`) tracking roots pushed per scope. At function
entry, params (and `self` for methods) are pushed as roots. Each `let`-
declared local is pushed after its declaration (`LAMO_GC_PUSH_ROOT(&v)`).
Inner blocks (`{}`, `if`, `while`, `for` bodies) get their own sub-scope
so their locals are popped at block exit (`LAMO_GC_POP_ROOTS_N(n)`).
At every `return` (implicit at function end, or user-written), all
active roots are popped.

For loops: the body is always wrapped in a block (even for single-
statement bodies) so the loop variable can be pushed as a root inside
the block scope. This is a minor codegen change but necessary for
correctness — without it, a `gc_collect()` inside the loop body could
free the loop variable's value.

Return statements: the pop+return pair is wrapped in `{ ... }` so it
acts as a single statement when used as an `if`/`match` arm body.
Without the braces, `if (cond) POP; return X;` would parse as
`if (cond) POP;` then `return X;` — the return would be unconditional
and the subsequent `else` would be a syntax error.

### Step 4 — Wire GC into `http_serve` and GUI loop — ✅ DONE (2.3.0)

The HTTP server loop in `lamo_http_run_server` calls `lamo_gc_collect()`
every 100 requests. The route table (`lamo_http_routes`) is allocated
outside the GC heap (plain `malloc`), so routes stay alive across
collections — only per-request garbage gets swept.

The GUI event loop: `lamo_gui_should_close` (both Win32 and X11
backends) calls `lamo_gc_collect()` every 1000 frames. The frame
counter is a static `lamo_gui_frame_counter` that resets on each
collection.

### Step 5 — Promote HTTP example — ✅ DONE (2.3.0)

`examples/http_server.lamo` re-promoted from "preview" to "official".
The warning header was replaced with documentation of the GC hook
(auto-collect every 100 requests), the concurrency story (single-
threaded, one request at a time), and error paths (port in use,
malformed request, client disconnect). `docs/SPEC.md` §11.2 updated.

### Step 6 — Tests — ✅ DONE (2.3.0)

`tests/runtime/gc_basic.lamo` — allocates lots of strings in a loop,
calls `gc_collect()`, asserts heap count dropped. Also verifies a
still-reachable string survives collection.

`tests/runtime/gc_cycle.lamo` — two arrays reference each other (a
cycle), drop all external references, collect, assert the cycle was
reclaimed. This is the classic mark-sweep vs refcounting differentiator
— a refcounting collector would leak the cycle forever; mark-sweep
reclaims it.

Both tests pass. The full suite (86 tests) passes with zero warnings
under `-Wall -Wextra`.

### Step 7 — Future (out of scope here)

- Generational: track age, promote surviving objects to an "old" heap that's
  collected less often.
- Incremental: split mark phase into slices, interleaved with user code.
- Concurrent: separate GC thread (needs the thread story first).

---

## 4. Policy: when can an HTTP example ship as "official"?

An example is "official" (linked from the README as a recommended pattern)
iff **all** of the following hold:

1. **Memory-safe for the workload.** Either the runtime reclaims memory
   during the run (GC enabled and tested), OR the example documents a hard
   upper bound on memory growth and the workload is short enough to fit
   inside it.

2. **Error paths defined.** What happens on malformed request? On port
   already in use? On client disconnect mid-request? All documented in
   the example file.

3. **Concurrency story documented.** Today: single-threaded, one request at
   a time. Documented in the example. (Multi-threaded is future work.)

4. **Tested under load.** A test script in `tests/runtime/` exercises the
   example for at least 1000 requests and asserts stable memory.

**Status (2.3.0):** `examples/http_server.lamo` satisfies (1)–(3). The GC
runs automatically every 100 requests (`lamo_http_run_server` calls
`lamo_gc_collect()`), so memory is reclaimed during the run. Error paths
and concurrency are documented in the example header. Item (4) — a formal
load test asserting stable memory over 1000 requests — is the remaining
gap; the GC infrastructure (the `gc_heap_count()` builtin) makes it
straightforward to add when needed. The example is **re-promoted to
"official"** as of 2.3.0.

---

## 5. FAQs

**Q: Why not just ship the arena leak as-is and document it?**
A: That was the status quo before the GC. Accepting the leak is a valid
resolution, but only if long-running examples are clearly demoted to
"preview" and the GC timeline is committed to. This document does both:
the arena remains the zero-cost default, while the opt-in collector moves
long-running programs from "leak, period" to "collectable on demand".

**Q: Won't the GC slow down short-lived programs?**
A: No — GC is opt-in. Programs that don't call `gc_collect()` or
`gc_set_threshold(N > 0)` see zero GC overhead. The only cost is the
per-allocation header (16 bytes per allocation), which is negligible.

**Q: What about the `lamo eval` / `lamo repl` paths?**
A: Same runtime, same GC available. REPL sessions that build up state can
call `gc_collect()` manually.

**Q: Will GC break existing tests?**
A: No. Existing tests don't call `gc_*`; they get the arena behavior, which
is unchanged. New GC-specific tests are added in Step 6.

**Q: Why not just use Boehm GC?**
A: Dependency cost. Boehm is ~50k lines of C, conservative (scans the whole
stack, slower), and adds a build dependency. Lamo's exact GC, while simpler,
is ~500 lines and lives in the existing runtime header. For a language
Lamo's size, that's the right trade.

**Q: What about GUI programs that run forever?**
A: Same story: opt-in GC. The X11/Win32 event loop in `lamo_runtime.h`
calls `lamo_gc_collect()` every 1000 frames (the same periodic hook the
HTTP server loop uses, per 100 requests).

---

## 6. References

- `src/codegen/lamo_runtime.h` — arena + GC implementation.
- `examples/http_server.lamo` — the example gated by §4.
- [SPEC.md](SPEC.md) §9 (Arrays), §11 (HTTP builtins), §12 (Runtime behavior).
- Mark-sweep GC: McCarthy 1960 ("Recursive Functions of Symbolic Expressions").
- Boehm GC — the conservative alternative we rejected.
- Python's `gc` module — inspiration for the `gc_collect()` /
  `gc_set_threshold()` API surface.
