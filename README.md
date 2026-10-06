# FiberECS — Lock-Free Fiber-Based Job System & Archetypal ECS Core (C++20)

FiberECS is a header-first C++20 foundation for simulation and game workloads: a
lock-free, work-stealing job scheduler that runs tasks on fibers, paired with an
archetypal Entity-Component-System that stores components in cache-friendly
SoA chunks. Systems declare `Read<T>` / `Write<T>` access and are dispatched as
parallel per-chunk jobs; a process-wide access gate serialises only the
dispatches that actually conflict.

## Performance

Measured on 100,000 entities, 3 systems per frame, 20 consecutive Release runs
(GCC 15.2.0, MinGW UCRT64, 8 workers, 7500 chunk jobs per measured window):

| Metric | Result |
|---|---|
| **Frame time @ 100k entities** | **0.326 ms** (min 0.252 ms / max 0.478 ms) |
| **Heap allocations in timed section** | **0** (zero-alloc steady state) |
| **Memory bandwidth** | **15.86 GB/s** peak |
| **Unit tests** | **59/59 passed** (Release **and** Debug) |
| **Memory leaks / correctness** | **0 leaks**, **bit-exact** verification |

Every timed frame touches a 3.9 MiB working set (40 B of traffic per entity per
frame) and every entity value is compared for exact equality against a serial
replay of the same floating-point operations — a mismatch fails the benchmark.

## Architectural highlights

### Fiber work-stealing scheduler
- Per-worker lock-free deques with randomized stealing; a worker that runs out
  of local work steals from its peers instead of idling.
- Tasks execute on fibers with a hand-written x86-64 context switch, so a
  blocking `wait(counter)` yields the worker's slot to other jobs rather than
  burning a thread. `wait()` first attempts to steal, then drains the
  injection queue.
- Jobs are submitted in blocks (`kJobBlockBytes = 256 KB` batches), counters
  are armed by the submitter, and quiescence detection (`drain()`) is what lets
  the arena rewind deterministically between frames.
- Systems that declare disjoint access run concurrently; conflicting systems
  are ordered by the access gate with no central work queue in the hot path.

### Archetype SoA memory layout, 64 B cache-line aligned
- Entities live in archetypes keyed by a component `Signature`; each archetype
  owns flat, fixed-capacity chunks of contiguous per-component columns —
  pointer arithmetic is `base + row * sizeof(T)`, with no indirection per row.
- The chunk header is `alignas(64)` (statically asserted), every column is
  padded to the next cache line, and column offsets/size are resolved once per
  archetype, so a 4-component row stays inside a handful of lines.
- Structural operations (`spawn`, `destroy`, `add<T>`, `remove<T>`) are
  archetype transitions: shared columns are copied across, the swap-and-pop
  repair keeps `Entity → {archetype, row}` locations exact, and generations
  make stale handles fail validation instead of aliasing live data.
- Queries return matching chunks once; dispatch submits exactly one job per
  non-empty chunk (100k entities ⇒ 250 chunks ⇒ 7500 jobs per benchmark
  window), so scheduling overhead scales with data, not with entities.

### Zero-alloc arenas
- Chunk blocks, batch job blocks and task/chunk scratch all come from
  `LinearArena` / `BlockArena` storage with pre-checked, alignment-aware
  allocation; the arenas never free per-frame, they rewind in O(1).
- A steady-state frame performs **zero** `operator new` calls — the benchmark
  replaces the global allocator and proves it: 0 allocations and 0 bytes across
  all timed windows.
- First-call costs (signature/access statics, scratch buffers, the first
  256 KB batch block) are paid in warmup; `drain()` before a timed frame
  guarantees the batch arena is fully rewound, which makes the zero-allocation
  claim deterministic rather than a race with the last job's bookkeeping.
- The shipped headless audit independently bounds runtime heap use to ≤ 64
  allocations (bounded arena expansion) and reports **0 memory leaks**.

## Architectural constraints & trade-offs

### Runtime archetype churn is the expensive path
Archetype transitions are correctness-first and cost far more than plain SoA
element access:

- `add<T>` / `remove<T>` / `destroy()` rebuild the row: append into the
  destination archetype, `copy_row_from` every shared column, `remove_at` in the
  source (swap-and-pop), then repair the displaced row's location. That is
  several cache-miss-heavy passes over the row instead of one.
- Each transition may allocate/attach a chunk on the arena and resolve column
  metadata, so bulk structural changes (adding a component to 100k entities in
  a loop) can dominate the frame — churn should be amortised, batched or moved
  to a phase boundary.
- Archetypes are retained once created. Memory for an emptied archetype is not
  reclaimed immediately (the tail chunk is popped and the archetype stops
  matching queries, but the archetype record and its other chunks stay), so a
  long run with highly variable signatures trades a little resident memory for
  O(1)-style structural edits and stable column layouts.
- `destroy()` is O(1) per entity plus a location repair, but a wave of
  destruction still dirties every touched chunk and can evict the working set.

Design rule: entities change *shape* rarely and change *values* constantly.

### Floating-point order is non-deterministic under parallel dispatch
- A system's body runs concurrently across chunks; completion order depends on
  worker scheduling, stealing and OS preemption. Any computation that mixes
  results *between* entities or chunks (reductions, prefix sums, accumulators,
  physics constraint loops that feed each other) therefore observes a
  different summation order every run, and IEEE-754 addition is not
  associative — the same inputs can yield a different last-ulp result.
- Per-entity updates are safe: each row's values derive from a fixed,
  single-threaded sequence of operations, which is exactly why the benchmark
  can verify bit-exact equality against a serial replay.
- Consequences to design around: do not expect reproducible builds of
  cross-entity aggregates from parallel dispatch alone; pin order-sensitive
  phases to a single job, a deterministic reduction pass, or fixed-point math.
  Conflict serialisation by the access gate fixes *which* systems may overlap,
  not *in which order* chunks of one system complete.

## Building and testing

Requirements: **CMake ≥ 3.20**, a **C++20** compiler (GCC 15 / Clang / MSVC
2022), and `make` or Ninja. CTest ships with CMake. The suite uses Catch2 or
GoogleTest when found and otherwise falls back to its built-in micro-harness —
no third-party download is required.

### Configure and build

```sh
# Release (default build type)
cmake -S . -B build
cmake --build build -j4

# Debug
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j4
```

Sanitizers are probed at configure time; ASan/UBSan are enabled only when the
toolchain actually ships the runtimes (MinGW-w64 does not, so the build
continues without them). MSVC C++20 presets are available through
`CMakePresets.json` (`cmake --preset release-msvc`, requires the matching
generator).

Binaries land in `build/bin/`:

| Binary | Purpose |
|---|---|
| `fiberecs_tests` | unit-test suite (59 cases) |
| `fiberecs` | demo/CLI + `--headless-test` leak and lifetime audit |
| `ecs_archetype_bench` | 100k-entity throughput benchmark (optional `[entity_count]`) |

### Run directly

```sh
./build/bin/fiberecs_tests.exe
./build/bin/fiberecs.exe --headless-test
./build/bin/ecs_archetype_bench.exe          # optional: ecs_archetype_bench.exe 250000
```

The benchmark exits non-zero if results are not bit-exact, if the timed section
touches the heap, or if the average frame exceeds the 5 ms target.

### Run with CTest

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build-debug --output-on-failure
```

Expected output: `100% tests passed out of 3` (`fiberecs_tests`,
`ecs_archetype_bench`, `headless_smoke`).

## Repository layout

```
core/       arenas (Linear/Block/Ring), memory utilities, fiber context switch
job/        work-stealing queues + JobScheduler (fiber job system)
include/    ecs/ — archetype.hpp, world.hpp, dispatcher.hpp (header-only ECS)
tests/      unit tests, built-in micro-harness, ECS benchmark
main.cpp    demo entry point + --headless-test audit
```

## License

MIT — see [LICENSE](LICENSE).
