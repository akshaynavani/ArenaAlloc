// Thin wrapper over the OS page allocator. None of these functions call malloc, so
// they are safe to use from inside the LD_PRELOAD shim.
#pragma once

#include <cstddef>

namespace arena::os {

// System page size (cached after the first call).
std::size_t page_size();

// Map `len` bytes of zeroed, read/write anonymous memory. Returns nullptr on failure.
void* map_pages(std::size_t len);

// Return a mapping obtained from map_pages().
void unmap_pages(void* addr, std::size_t len);

// Write a message to stderr and abort. Uses write(2) only, never malloc.
[[noreturn]] void fatal(const char* msg);

}  // namespace arena::os
