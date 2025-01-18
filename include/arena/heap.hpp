// A single segregated-fit arena.
//
// Memory comes in chunks, either mmap'd from the OS or one caller-supplied buffer:
//
//   [ChunkHeader 16B][block][block]...[block][epilogue 16B]
//
// The first block of a chunk always has PREV_INUSE set, and the epilogue is a 0-sized
// "in use" header. Together they stop coalescing at chunk boundaries.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "arena/block.hpp"
#include "arena/size_class.hpp"
#include "arena/stats.hpp"

namespace arena {

struct HeapConfig {
  std::size_t chunk_size = std::size_t{1} << 20;     // granularity of OS requests
  std::size_t mmap_threshold = 256 * 1024;           // blocks >= this go straight to mmap
  std::size_t max_footprint = 0;                     // 0 = unlimited
  bool release_empty_chunks = true;                  // unmap fully free chunks (keeps one)
};

class Heap {
 public:
  explicit Heap(std::uint32_t id = 0, const HeapConfig& cfg = HeapConfig{});
  // Embedded mode: carve the whole heap out of `buffer` and never call the OS.
  Heap(void* buffer, std::size_t len, std::uint32_t id = 0);
  ~Heap();

  Heap(const Heap&) = delete;
  Heap& operator=(const Heap&) = delete;

  // Thread-safe entry points (each takes the heap mutex).
  void* allocate(std::size_t n);
  void* allocate_aligned(std::size_t alignment, std::size_t n);
  void deallocate(void* p);
  void* reallocate(void* p, std::size_t n);

  // Like allocate(), but gives up immediately if the mutex is held by another thread.
  // Sets *contended and returns nullptr in that case.
  void* try_allocate(std::size_t n, bool* contended);

  HeapStats stats() const;
  // Walks every chunk and bin, checking all invariants. Returns false and fills `err`
  // (when non-null) on the first violation found.
  bool validate(std::string* err = nullptr) const;

  std::uint32_t id() const { return id_; }
  const HeapConfig& config() const { return cfg_; }

  // Usable payload bytes of an allocated pointer (any heap).
  static std::size_t usable_size(const void* p);
  // Owner arena id recorded in the block header; aborts on a corrupt pointer.
  static std::uint32_t owner_of(const void* p);
  static bool is_mmapped_ptr(const void* p);

 private:
  struct ChunkHeader {
    ChunkHeader* next;
    std::size_t size;
  };
  static constexpr std::size_t kChunkHeaderSize = 16;
  static constexpr std::size_t kEpilogueSize = kHeaderSize;
  static_assert(sizeof(ChunkHeader) == kChunkHeaderSize, "chunk header must be 16 bytes");

  // Unlocked internals; the caller holds mu_.
  void* allocate_unlocked(std::size_t n);
  void* allocate_aligned_unlocked(std::size_t alignment, std::size_t n);
  void deallocate_unlocked(void* p);
  void* reallocate_unlocked(void* p, std::size_t n);

  BlockHeader* find_fit(std::size_t bs);
  bool grow(std::size_t bs);
  void init_chunk(void* mem, std::size_t len);
  void insert_free(BlockHeader* b);
  void remove_free(BlockHeader* b);
  void mark_in_use(BlockHeader* b);
  void split_tail(BlockHeader* b, std::size_t keep);
  void release_block(BlockHeader* b);
  bool try_release_chunk(BlockHeader* b);
  void set_header(BlockHeader* b, std::size_t size, std::uint64_t flags);

  void* allocate_mmapped(std::size_t n, std::size_t alignment);
  void free_mmapped(BlockHeader* b);

  void note_footprint_change(std::ptrdiff_t delta);

  std::uint32_t id_;
  HeapConfig cfg_;
  bool owns_memory_;

  BlockHeader* bins_[kNumBins] = {};
  BinBitmap bitmap_;
  ChunkHeader* chunks_ = nullptr;

  HeapStats stats_;
  mutable std::mutex mu_;
};

}  // namespace arena
