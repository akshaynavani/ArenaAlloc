/*
 * LD_PRELOAD interposer: routes the standard allocation functions to ArenaAlloc.
 *
 *   LD_PRELOAD=./build/libarenaalloc_preload.so ./your_program
 *
 * Linux/glibc only. macOS needs malloc-zone registration instead, which is out of
 * scope. This file deliberately avoids <stdlib.h>/<malloc.h> so its definitions don't
 * clash with the libc prototypes.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "arena/arena_alloc.h"

#define SHIM_EXPORT __attribute__((visibility("default")))

static size_t shim_page_size(void) {
  long ps = sysconf(_SC_PAGESIZE);
  return ps > 0 ? (size_t)ps : 4096;
}

SHIM_EXPORT void* malloc(size_t size) {
  void* p = arena_malloc(size);
  if (p == NULL) errno = ENOMEM;
  return p;
}

SHIM_EXPORT void free(void* ptr) { arena_free(ptr); }

SHIM_EXPORT void cfree(void* ptr) { arena_free(ptr); }

SHIM_EXPORT void* calloc(size_t count, size_t size) {
  void* p = arena_calloc(count, size);
  if (p == NULL) errno = ENOMEM;
  return p;
}

SHIM_EXPORT void* realloc(void* ptr, size_t size) {
  void* p = arena_realloc(ptr, size);
  if (p == NULL && size != 0) errno = ENOMEM;
  return p;
}

SHIM_EXPORT void* reallocarray(void* ptr, size_t count, size_t size) {
  size_t total;
  if (__builtin_mul_overflow(count, size, &total)) {
    errno = ENOMEM;
    return NULL;
  }
  return realloc(ptr, total);
}

SHIM_EXPORT int posix_memalign(void** out, size_t alignment, size_t size) {
  if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) return EINVAL;
  void* p = arena_aligned_alloc(alignment, size);
  if (p == NULL) return ENOMEM;
  *out = p;
  return 0;
}

SHIM_EXPORT void* aligned_alloc(size_t alignment, size_t size) {
  void* p = arena_aligned_alloc(alignment, size);
  if (p == NULL) errno = (alignment & (alignment - 1)) ? EINVAL : ENOMEM;
  return p;
}

SHIM_EXPORT void* memalign(size_t alignment, size_t size) {
  return aligned_alloc(alignment, size);
}

SHIM_EXPORT void* valloc(size_t size) { return aligned_alloc(shim_page_size(), size); }

SHIM_EXPORT void* pvalloc(size_t size) {
  const size_t ps = shim_page_size();
  const size_t rounded = (size + ps - 1) & ~(ps - 1);
  return aligned_alloc(ps, rounded == 0 ? ps : rounded);
}

SHIM_EXPORT size_t malloc_usable_size(void* ptr) { return arena_usable_size(ptr); }
