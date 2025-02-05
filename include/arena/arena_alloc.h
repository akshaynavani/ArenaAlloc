/* C interface to the process-wide ArenaAlloc pool. */
#ifndef ARENAALLOC_ARENA_ALLOC_H
#define ARENAALLOC_ARENA_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct arena_stats {
  size_t footprint;
  size_t peak_footprint;
  size_t in_use_bytes;
  size_t free_bytes;
  size_t largest_free_block;
  size_t mmapped_bytes;
  size_t num_chunks;
  size_t free_block_count;
  unsigned long long alloc_count;
  unsigned long long free_count;
  unsigned long long failed_allocs;
  double external_fragmentation;
} arena_stats;

void* arena_malloc(size_t size);
void arena_free(void* ptr);
void* arena_calloc(size_t count, size_t size);
void* arena_realloc(void* ptr, size_t size);
/* alignment must be a power of two; returns NULL otherwise. */
void* arena_aligned_alloc(size_t alignment, size_t size);
size_t arena_usable_size(const void* ptr);

/* Aggregated statistics across all arenas of the global pool. */
void arena_get_stats(arena_stats* out);
/* Returns 1 if every arena passes its consistency check, 0 otherwise. */
int arena_validate(void);

#ifdef __cplusplus
}
#endif

#endif /* ARENAALLOC_ARENA_ALLOC_H */
