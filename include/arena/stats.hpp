#pragma once

#include <cstddef>
#include <cstdint>

namespace arena {

struct HeapStats {
  std::size_t footprint = 0;           // bytes obtained from the OS (chunks + direct mmaps)
  std::size_t peak_footprint = 0;      // high-water mark of `footprint`
  std::size_t in_use_bytes = 0;        // bytes in allocated blocks (including headers)
  std::size_t free_bytes = 0;          // bytes in free-list blocks
  std::size_t largest_free_block = 0;  // size of the biggest free block
  std::size_t mmapped_bytes = 0;       // bytes in direct-mmap large blocks
  std::size_t num_chunks = 0;          // number of live chunks / backing buffers
  std::size_t free_block_count = 0;    // number of blocks across all bins
  std::uint64_t alloc_count = 0;
  std::uint64_t free_count = 0;
  std::uint64_t failed_allocs = 0;

  // 1 - largest_free / total_free: 0 means all free memory is one contiguous block,
  // and a value close to 1 means the free memory is scattered across many small holes.
  double external_fragmentation() const {
    if (free_bytes == 0) return 0.0;
    return 1.0 - static_cast<double>(largest_free_block) / static_cast<double>(free_bytes);
  }

  // Share of the OS footprint that is not holding allocated blocks.
  double footprint_overhead() const {
    if (footprint == 0) return 0.0;
    return 1.0 - static_cast<double>(in_use_bytes) / static_cast<double>(footprint);
  }

  HeapStats& operator+=(const HeapStats& o) {
    footprint += o.footprint;
    peak_footprint += o.peak_footprint;
    in_use_bytes += o.in_use_bytes;
    free_bytes += o.free_bytes;
    if (o.largest_free_block > largest_free_block) largest_free_block = o.largest_free_block;
    mmapped_bytes += o.mmapped_bytes;
    num_chunks += o.num_chunks;
    free_block_count += o.free_block_count;
    alloc_count += o.alloc_count;
    free_count += o.free_count;
    failed_allocs += o.failed_allocs;
    return *this;
  }
};

}  // namespace arena
