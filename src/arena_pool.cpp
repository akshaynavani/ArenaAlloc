#include "arena/arena_pool.hpp"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <new>

#include "arena/os_memory.hpp"

namespace arena {

namespace {

constexpr std::uint32_t kIndexBits = 8;
constexpr std::uint32_t kIndexMask = (1u << kIndexBits) - 1;
static_assert(ArenaPool::kMaxArenas <= (1u << kIndexBits), "arena index must fit in id");

constexpr std::uint32_t kUnassigned = ~std::uint32_t{0};

std::atomic<std::uint32_t> g_next_pool_tag{1};
std::atomic<std::uint32_t> g_next_thread_slot{0};

// Trivially initialised so touching it never allocates (important under LD_PRELOAD).
thread_local std::uint32_t t_slot = kUnassigned;

std::uint32_t thread_slot() {
  if (t_slot == kUnassigned) {
    t_slot = g_next_thread_slot.fetch_add(1, std::memory_order_relaxed);
  }
  return t_slot;
}

std::size_t online_cpus() {
  long n = ::sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? static_cast<std::size_t>(n) : 1;
}

// strtoul-style parse that ignores junk; returns 0 when unset/invalid.
std::size_t env_size(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') return 0;
  char* end = nullptr;
  unsigned long long x = std::strtoull(v, &end, 10);
  return (end != v) ? static_cast<std::size_t>(x) : 0;
}

}  // namespace

ArenaPool::ArenaPool(const PoolConfig& cfg) {
  std::size_t n = cfg.num_arenas != 0 ? cfg.num_arenas : online_cpus();
  if (n > kMaxArenas) n = kMaxArenas;
  if (n == 0) n = 1;
  n_ = n;
  tag_ = g_next_pool_tag.fetch_add(1, std::memory_order_relaxed);
  for (std::size_t i = 0; i < n_; ++i) {
    const std::uint32_t id = (tag_ << kIndexBits) | static_cast<std::uint32_t>(i);
    new (storage_[i]) Heap(id, cfg.heap);
  }
}

ArenaPool::~ArenaPool() {
  for (std::size_t i = 0; i < n_; ++i) heap_at(i)->~Heap();
}

Heap& ArenaPool::home() { return *heap_at(thread_slot() % n_); }

Heap& ArenaPool::owner(const void* p) {
  const std::uint32_t id = Heap::owner_of(p);
  const std::uint32_t idx = id & kIndexMask;
  if ((id >> kIndexBits) != tag_ || idx >= n_) {
    os::fatal("pointer was not allocated by this arena pool");
  }
  return *heap_at(idx);
}

void* ArenaPool::allocate(std::size_t n) {
  bool contended = false;
  void* p = home().try_allocate(n, &contended);
  if (!contended) return p;
  // Home arena is busy: migrate this thread to the next arena and block there.
  t_slot = thread_slot() + 1;
  return home().allocate(n);
}

void* ArenaPool::allocate_zeroed(std::size_t count, std::size_t size) {
  std::size_t total = 0;
  if (__builtin_mul_overflow(count, size, &total)) return nullptr;
  void* p = allocate(total);
  // Fresh direct mappings are already zero-filled by the kernel.
  if (p != nullptr && !Heap::is_mmapped_ptr(p)) std::memset(p, 0, total);
  return p;
}

void* ArenaPool::allocate_aligned(std::size_t alignment, std::size_t n) {
  return home().allocate_aligned(alignment, n);
}

void* ArenaPool::reallocate(void* p, std::size_t n) {
  if (p == nullptr) return allocate(n);
  if (n == 0) {
    deallocate(p);
    return nullptr;
  }
  return owner(p).reallocate(p, n);
}

void ArenaPool::deallocate(void* p) {
  if (p == nullptr) return;
  owner(p).deallocate(p);
}

HeapStats ArenaPool::stats() const {
  HeapStats total;
  for (std::size_t i = 0; i < n_; ++i) total += heap_at(i)->stats();
  return total;
}

bool ArenaPool::validate(std::string* err) const {
  for (std::size_t i = 0; i < n_; ++i) {
    if (!heap_at(i)->validate(err)) return false;
  }
  return true;
}

PoolConfig ArenaPool::config_from_env() {
  PoolConfig cfg;
  cfg.num_arenas = env_size("ARENAALLOC_ARENAS");
  if (std::size_t kb = env_size("ARENAALLOC_CHUNK_KB")) cfg.heap.chunk_size = kb * 1024;
  if (std::size_t kb = env_size("ARENAALLOC_MMAP_THRESHOLD_KB")) {
    cfg.heap.mmap_threshold = kb * 1024;
  }
  return cfg;
}

ArenaPool& ArenaPool::global() {
  alignas(ArenaPool) static unsigned char storage[sizeof(ArenaPool)];
  static ArenaPool* pool = new (storage) ArenaPool(config_from_env());
  return *pool;
}

}  // namespace arena
