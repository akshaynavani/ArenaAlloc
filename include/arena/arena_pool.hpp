// A pool of independent arenas so threads don't all serialise on one lock.
//
// Each thread has a "home" arena. An allocation tries the home arena with try_lock,
// and if that arena is busy the thread moves on to the next one. That spreads hot
// threads across arenas the way glibc's arena selection does. A free always goes
// back to the arena that owns the block. The owner is found from the arena id in the
// block header, so freeing a block on a different thread than the one that allocated
// it is safe.
//
// The pool stores its heaps inline (no heap allocation of its own), which keeps it
// usable as the process-wide malloc.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "arena/heap.hpp"
#include "arena/stats.hpp"

namespace arena {

struct PoolConfig {
  std::size_t num_arenas = 0;  // 0 = one per online CPU (capped at kMaxArenas)
  HeapConfig heap{};
};

class ArenaPool {
 public:
  static constexpr std::size_t kMaxArenas = 64;

  explicit ArenaPool(const PoolConfig& cfg = PoolConfig{});
  ~ArenaPool();

  ArenaPool(const ArenaPool&) = delete;
  ArenaPool& operator=(const ArenaPool&) = delete;

  void* allocate(std::size_t n);
  void* allocate_zeroed(std::size_t count, std::size_t size);
  void* allocate_aligned(std::size_t alignment, std::size_t n);
  void* reallocate(void* p, std::size_t n);
  void deallocate(void* p);
  static std::size_t usable_size(const void* p) { return Heap::usable_size(p); }

  std::size_t num_arenas() const { return n_; }
  Heap& arena(std::size_t i) { return *heap_at(i); }
  const Heap& arena(std::size_t i) const { return *heap_at(i); }

  HeapStats stats() const;
  bool validate(std::string* err = nullptr) const;

  // Process-wide pool, built on first use from environment variables:
  //   ARENAALLOC_ARENAS             number of arenas
  //   ARENAALLOC_CHUNK_KB           chunk size in KiB
  //   ARENAALLOC_MMAP_THRESHOLD_KB  direct-mmap threshold in KiB
  // It is never destroyed, so frees during static destruction stay valid.
  static ArenaPool& global();
  static PoolConfig config_from_env();

 private:
  Heap* heap_at(std::size_t i) { return reinterpret_cast<Heap*>(storage_[i]); }
  const Heap* heap_at(std::size_t i) const { return reinterpret_cast<const Heap*>(storage_[i]); }
  Heap& home();
  Heap& owner(const void* p);

  alignas(Heap) unsigned char storage_[kMaxArenas][sizeof(Heap)];
  std::size_t n_ = 0;
  std::uint32_t tag_ = 0;  // high bits of every arena id in this pool
};

}  // namespace arena
