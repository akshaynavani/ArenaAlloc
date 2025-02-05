#include "arena/arena_alloc.h"

#include "arena/arena_pool.hpp"

using arena::ArenaPool;

extern "C" {

void* arena_malloc(size_t size) { return ArenaPool::global().allocate(size); }

void arena_free(void* ptr) {
  if (ptr != nullptr) ArenaPool::global().deallocate(ptr);
}

void* arena_calloc(size_t count, size_t size) {
  return ArenaPool::global().allocate_zeroed(count, size);
}

void* arena_realloc(void* ptr, size_t size) { return ArenaPool::global().reallocate(ptr, size); }

void* arena_aligned_alloc(size_t alignment, size_t size) {
  return ArenaPool::global().allocate_aligned(alignment, size);
}

size_t arena_usable_size(const void* ptr) { return ArenaPool::usable_size(ptr); }

void arena_get_stats(arena_stats* out) {
  if (out == nullptr) return;
  const arena::HeapStats s = ArenaPool::global().stats();
  out->footprint = s.footprint;
  out->peak_footprint = s.peak_footprint;
  out->in_use_bytes = s.in_use_bytes;
  out->free_bytes = s.free_bytes;
  out->largest_free_block = s.largest_free_block;
  out->mmapped_bytes = s.mmapped_bytes;
  out->num_chunks = s.num_chunks;
  out->free_block_count = s.free_block_count;
  out->alloc_count = s.alloc_count;
  out->free_count = s.free_count;
  out->failed_allocs = s.failed_allocs;
  out->external_fragmentation = s.external_fragmentation();
}

int arena_validate(void) { return ArenaPool::global().validate(nullptr) ? 1 : 0; }

}  // extern "C"
