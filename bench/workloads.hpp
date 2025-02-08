// Deterministic allocation traces that mimic embedded / firmware-style workloads.
//
// A trace is a flat list of operations over numbered "slots": an op with size > 0
// allocates into the slot, and an op with size == 0 frees whatever the slot holds.
// The traces are generated before timing starts, so every allocator replays exactly the
// same sequence. Each trace ends by freeing everything still live.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bench {

struct Op {
  std::uint32_t slot;
  std::uint32_t size;  // 0 = free
};

struct Trace {
  std::string name;
  std::vector<Op> ops;
  std::uint32_t num_slots = 0;
  std::size_t peak_live_bytes = 0;  // as requested, i.e. without allocator overhead
};

// sensor   - many small fixed-type packets (24..256 B) with random short lifetimes
// mixed    - bimodal message buffers: mostly 16..128 B, some 1..8 KiB
// sawtooth - repeated build-up/tear-down cycles that leave long-lived survivors
//            scattered through the heap, with the size mix shifting between cycles
// frames   - a ring of large video/DMA frames (16..64 KiB) interleaved with
//            small control structures
Trace make_trace(const std::string& workload, std::uint64_t seed, std::size_t target_ops);

const std::vector<std::string>& workload_names();

}  // namespace bench
