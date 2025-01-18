#include "arena/os_memory.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

namespace arena::os {

std::size_t page_size() {
  static std::atomic<std::size_t> cached{0};
  std::size_t ps = cached.load(std::memory_order_relaxed);
  if (ps == 0) {
    long v = ::sysconf(_SC_PAGESIZE);
    ps = v > 0 ? static_cast<std::size_t>(v) : 4096;
    cached.store(ps, std::memory_order_relaxed);
  }
  return ps;
}

void* map_pages(std::size_t len) {
  void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

void unmap_pages(void* addr, std::size_t len) {
  if (addr != nullptr && len != 0) {
    ::munmap(addr, len);
  }
}

void fatal(const char* msg) {
  static const char prefix[] = "arenaalloc: fatal: ";
  ssize_t r = ::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
  r = ::write(STDERR_FILENO, msg, std::strlen(msg));
  r = ::write(STDERR_FILENO, "\n", 1);
  (void)r;
  std::abort();
}

}  // namespace arena::os
