# ArenaAlloc: Implementation Plan

**ArenaAlloc** is a thread-safe heap allocator written in C++17. It uses segregated free lists,
boundary-tag coalescing and mutex-protected arenas, and it ships with a benchmark harness
that compares it against the system `malloc` on simulated embedded workloads. The
benchmarks measure throughput and fragmentation.

## Goals

- A drop-in `malloc`/`free`/`calloc`/`realloc`/`aligned_alloc` replacement, available
  through a C API and an `LD_PRELOAD` shim on Linux.
- Segregated-fit allocation: 62 exact-size small bins (48 B to 1 KiB, 16 B steps), plus
  log-spaced large bins with 4 sub-bins per power of two. A bitmap finds the next
  non-empty bin.
- Immediate coalescing with boundary tags (header plus footer on free blocks, and a
  `PREV_INUSE` bit), so no two free blocks are ever adjacent.
- Thread safety: several independent arenas (heaps), each guarded by its own mutex.
  Threads stick to an arena and migrate when they hit contention. A `free` from another
  thread is sent to the owning arena through an arena id stored in the block header.
- An embedded mode: a heap built on a caller-supplied static buffer, which never calls
  into the OS.
- Observability: stats (footprint, in-use bytes, free bytes, largest free block, external
  fragmentation) and a full `validate()` heap-consistency checker.

## Key design decisions

| Decision | Choice | Why |
|---|---|---|
| Language/build | C++17 + CMake, GoogleTest via FetchContent | Portable, standard tooling |
| Backing memory | `mmap` chunks (default 1 MiB), or a static buffer | Works for both embedded and hosted use |
| Block header | 16 B: `size\|flags`, `arena_id`, `magic` | Keeps 16 B alignment and lets free be routed and checked |
| Min block | 48 B (header + 2 links + footer, rounded) | Free blocks must hold their list links and footer |
| Large requests | Direct `mmap` above 256 KiB | Avoids polluting the bins with huge blocks |
| Fit policy | Exact for small bins, best fit within a large bin, else first non-empty higher bin | Low fragmentation at near-O(1) cost |
| Locking | One `std::mutex` per arena, `try_lock` and migration on contention | Simple critical sections that are easy to reason about |

## Phases

### Phase 1: Foundations
- CMake project and library target `arenaalloc`.
- `include/arena/block.hpp`: block layout, flags, pointer helpers.
- `include/arena/size_class.hpp`: request-to-block rounding, bin indexing, bin bitmap.
- `include/arena/os_memory.hpp`, `src/os_memory.cpp`: page mapping (`mmap`/`munmap`).

### Phase 2: Single-arena segregated-fit heap
- `include/arena/heap.hpp`, `src/heap.cpp`: chunk management (prologue and epilogue),
  bin insert and remove, find-fit, split, free with coalescing, release of empty chunks,
  direct-mmap large blocks, fixed-buffer constructor.
- `include/arena/stats.hpp`: `HeapStats` and the fragmentation metrics.
- `Heap::validate()` invariant checker.

### Phase 3: Thread safety and public API
- A per-heap mutex and `try_allocate` for contention detection.
- `realloc` (in-place grow and shrink), aligned allocation, `calloc`.
- `include/arena/arena_pool.hpp`, `src/arena_pool.cpp`: multi-arena pool, thread-to-arena
  assignment, cross-thread free routing, environment-variable configuration.
- `include/arena/arena_alloc.h`, `src/c_api.cpp`: the C API.
- `src/malloc_shim.cpp`: `LD_PRELOAD` interposer (Linux).

### Phase 4: Test suite
- `tests/`: GoogleTest suites for size classes, basic alloc/free, split and coalesce,
  realloc and aligned allocation, the fixed-buffer heap, multi-threaded stress with
  cross-thread frees, and the C API.

### Phase 5: Benchmarks and fragmentation analysis
- `bench/workloads.{hpp,cpp}`: deterministic trace generators for embedded-style
  workloads (sensor packets, mixed message buffers, sawtooth with long-lived survivors,
  video frames).
- `bench/allocators.hpp`: adapters for the system malloc and ArenaAlloc, including OS
  footprint probes.
- `bench/frag_bench.cpp`: fragmentation and footprint comparison.
- `bench/throughput_bench.cpp`: multi-threaded throughput.
- `bench/run_benchmarks.sh`: runs every combination and prints a table.

## After implementation
- `.gitignore` for CMake/C++ builds.
- `README.md` with an architecture diagram, build/run/test instructions and benchmark usage.
