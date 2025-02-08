// Allocator adapters used by the benchmarks, plus the probes that read the system
// allocator's OS footprint.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include "arena/arena_pool.hpp"
#include "arena/heap.hpp"

#if defined(__GLIBC__)
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif

namespace bench {

class Allocator {
 public:
  virtual ~Allocator() = default;
  virtual const char* name() const = 0;
  virtual void* alloc(std::size_t n) = 0;
  virtual void release(void* p) = 0;
  // Bytes this allocator currently holds from the OS (0 if it can't be measured).
  virtual std::size_t footprint() const = 0;
};

// Footprint of the libc allocator, as the allocator itself reports it.
inline std::size_t system_malloc_footprint() {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  struct mallinfo2 mi = mallinfo2();
  return mi.arena + mi.hblkhd;  // sbrk'd/arena bytes + mmapped chunks
#elif defined(__GLIBC__)
  struct mallinfo mi = mallinfo();
  return static_cast<std::size_t>(static_cast<unsigned>(mi.arena)) +
         static_cast<std::size_t>(static_cast<unsigned>(mi.hblkhd));
#elif defined(__APPLE__)
  malloc_statistics_t st;
  malloc_zone_statistics(nullptr, &st);
  return st.size_allocated;
#else
  return 0;
#endif
}

class SystemAllocator final : public Allocator {
 public:
  const char* name() const override { return "system"; }
  void* alloc(std::size_t n) override { return std::malloc(n); }
  void release(void* p) override { std::free(p); }
  std::size_t footprint() const override { return system_malloc_footprint(); }
};

// A single ArenaAlloc heap: the configuration used for embedded-style fragmentation runs.
class ArenaHeapAllocator final : public Allocator {
 public:
  explicit ArenaHeapAllocator(const arena::HeapConfig& cfg) : heap_(0, cfg) {}
  const char* name() const override { return "arena"; }
  void* alloc(std::size_t n) override { return heap_.allocate(n); }
  void release(void* p) override { heap_.deallocate(p); }
  std::size_t footprint() const override { return heap_.stats().footprint; }
  const arena::Heap& heap() const { return heap_; }

 private:
  arena::Heap heap_;
};

// The multi-arena pool, used for the threaded throughput runs.
class ArenaPoolAllocator final : public Allocator {
 public:
  explicit ArenaPoolAllocator(const arena::PoolConfig& cfg)
      : pool_(std::make_unique<arena::ArenaPool>(cfg)) {}
  const char* name() const override { return "arena-pool"; }
  void* alloc(std::size_t n) override { return pool_->allocate(n); }
  void release(void* p) override { pool_->deallocate(p); }
  std::size_t footprint() const override { return pool_->stats().footprint; }
  const arena::ArenaPool& pool() const { return *pool_; }

 private:
  std::unique_ptr<arena::ArenaPool> pool_;
};

inline std::unique_ptr<Allocator> make_allocator(const std::string& kind,
                                                 const arena::HeapConfig& heap_cfg,
                                                 std::size_t arenas) {
  if (kind == "system") return std::make_unique<SystemAllocator>();
  if (kind == "arena") return std::make_unique<ArenaHeapAllocator>(heap_cfg);
  if (kind == "arena-pool") {
    arena::PoolConfig cfg;
    cfg.num_arenas = arenas;
    cfg.heap = heap_cfg;
    return std::make_unique<ArenaPoolAllocator>(cfg);
  }
  throw std::invalid_argument("unknown allocator: " + kind + " (system|arena|arena-pool)");
}

}  // namespace bench
