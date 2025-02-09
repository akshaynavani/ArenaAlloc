#include "arena/heap.hpp"

#include <cstdint>
#include <cstring>

#include "arena/os_memory.hpp"

namespace arena {

namespace {

inline std::uintptr_t addr(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

inline BlockHeader* at(void* base, std::size_t off) {
  return reinterpret_cast<BlockHeader*>(static_cast<char*>(base) + off);
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / chunk management
// ---------------------------------------------------------------------------

Heap::Heap(std::uint32_t id, const HeapConfig& cfg) : id_(id), cfg_(cfg), owns_memory_(true) {
  if (cfg_.chunk_size < os::page_size()) cfg_.chunk_size = os::page_size();
  cfg_.chunk_size = align_up(cfg_.chunk_size, os::page_size());
}

Heap::Heap(void* buffer, std::size_t len, std::uint32_t id) : id_(id), owns_memory_(false) {
  cfg_.chunk_size = 0;
  cfg_.mmap_threshold = ~std::size_t{0};  // never bypass the buffer
  cfg_.release_empty_chunks = false;

  const std::uintptr_t start = align_up(addr(buffer), kAlign);
  const std::uintptr_t end = (addr(buffer) + len) & ~(std::uintptr_t{kAlign} - 1);
  if (end <= start) return;
  const std::size_t usable = static_cast<std::size_t>(end - start);
  if (usable < kChunkHeaderSize + kMinBlock + kEpilogueSize) return;

  init_chunk(reinterpret_cast<void*>(start), usable);
  note_footprint_change(static_cast<std::ptrdiff_t>(usable));
}

Heap::~Heap() {
  if (!owns_memory_) return;
  ChunkHeader* c = chunks_;
  while (c != nullptr) {
    ChunkHeader* next = c->next;
    os::unmap_pages(c, c->size);
    c = next;
  }
  chunks_ = nullptr;
}

void Heap::set_header(BlockHeader* b, std::size_t size, std::uint64_t flags) {
  b->size_flags = static_cast<std::uint64_t>(size) | flags;
  b->arena_id = id_;
  b->magic = kMagic;
}

void Heap::init_chunk(void* mem, std::size_t len) {
  auto* c = static_cast<ChunkHeader*>(mem);
  c->next = chunks_;
  c->size = len;
  chunks_ = c;
  ++stats_.num_chunks;

  const std::size_t first_size = len - kChunkHeaderSize - kEpilogueSize;
  BlockHeader* first = at(mem, kChunkHeaderSize);
  set_header(first, first_size, kPrevInUse);
  write_footer(first);

  BlockHeader* epilogue = at(mem, len - kEpilogueSize);
  set_header(epilogue, 0, kInUse);  // PREV_INUSE clear: `first` is free

  insert_free(first);
}

bool Heap::grow(std::size_t bs) {
  if (!owns_memory_) return false;
  const std::size_t need = align_up(bs + kChunkHeaderSize + kEpilogueSize, os::page_size());
  const std::size_t len = need > cfg_.chunk_size ? need : cfg_.chunk_size;
  if (cfg_.max_footprint != 0 && stats_.footprint + len > cfg_.max_footprint) return false;

  void* mem = os::map_pages(len);
  if (mem == nullptr) return false;
  init_chunk(mem, len);
  note_footprint_change(static_cast<std::ptrdiff_t>(len));
  return true;
}

void Heap::note_footprint_change(std::ptrdiff_t delta) {
  stats_.footprint = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(stats_.footprint) + delta);
  if (stats_.footprint > stats_.peak_footprint) stats_.peak_footprint = stats_.footprint;
}

// ---------------------------------------------------------------------------
// Free lists
// ---------------------------------------------------------------------------

void Heap::insert_free(BlockHeader* b) {
  const std::size_t idx = bin_index(block_size(b));
  FreeLinks* l = links(b);
  l->prev = nullptr;
  l->next = bins_[idx];
  if (bins_[idx] != nullptr) links(bins_[idx])->prev = b;
  bins_[idx] = b;
  bitmap_.set(idx);
  stats_.free_bytes += block_size(b);
  ++stats_.free_block_count;
}

void Heap::remove_free(BlockHeader* b) {
  const std::size_t idx = bin_index(block_size(b));
  FreeLinks* l = links(b);
  if (l->prev != nullptr) {
    links(l->prev)->next = l->next;
  } else {
    bins_[idx] = l->next;
  }
  if (l->next != nullptr) links(l->next)->prev = l->prev;
  if (bins_[idx] == nullptr) bitmap_.clear(idx);
  stats_.free_bytes -= block_size(b);
  --stats_.free_block_count;
}

// Finds a free block of at least `bs` bytes and unlinks it from its bin.
BlockHeader* Heap::find_fit(std::size_t bs) {
  const std::size_t idx = bin_index(bs);

  if (is_small_bin(idx)) {
    // Small bins hold exactly one size, so the head is a perfect fit.
    if (BlockHeader* b = bins_[idx]) {
      remove_free(b);
      return b;
    }
  } else if (bins_[idx] != nullptr) {
    // Large bins hold a size range: best fit within the bin.
    BlockHeader* best = nullptr;
    for (BlockHeader* b = bins_[idx]; b != nullptr; b = links(b)->next) {
      const std::size_t sz = block_size(b);
      if (sz >= bs && (best == nullptr || sz < block_size(best))) {
        best = b;
        if (sz == bs) break;
      }
    }
    if (best != nullptr) {
      remove_free(best);
      return best;
    }
  }

  // Any block in a higher bin is strictly larger than bs.
  const std::size_t j = bitmap_.find_from(idx + 1);
  if (j >= kNumBins) return nullptr;
  BlockHeader* b = bins_[j];
  remove_free(b);
  return b;
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

// Marks a just-unlinked free block as allocated, keeping its whole size.
void Heap::mark_in_use(BlockHeader* b) {
  // A free block's predecessor is always in use (no adjacent free blocks).
  set_header(b, block_size(b), kInUse | kPrevInUse);
  next_block(b)->size_flags |= kPrevInUse;
  stats_.in_use_bytes += block_size(b);
}

// Shrinks an in-use block to `keep` bytes and frees the tail when it is big enough
// to be a block of its own.
void Heap::split_tail(BlockHeader* b, std::size_t keep) {
  const std::size_t cur = block_size(b);
  if (cur < keep + kMinBlock) return;
  const std::uint64_t prev_flag = b->size_flags & kPrevInUse;
  set_header(b, keep, kInUse | prev_flag);
  BlockHeader* rem = next_block(b);
  set_header(rem, cur - keep, kInUse | kPrevInUse);
  release_block(rem);
}

void* Heap::allocate_unlocked(std::size_t n) {
  if (n > kMaxRequest) {
    ++stats_.failed_allocs;
    return nullptr;
  }
  const std::size_t bs = request_to_block(n);
  if (owns_memory_ && bs >= cfg_.mmap_threshold) {
    return allocate_mmapped(n, kAlign);
  }

  BlockHeader* b = find_fit(bs);
  if (b == nullptr) {
    if (!grow(bs) || (b = find_fit(bs)) == nullptr) {
      ++stats_.failed_allocs;
      return nullptr;
    }
  }
  mark_in_use(b);
  split_tail(b, bs);
  ++stats_.alloc_count;
  return payload(b);
}

// ---------------------------------------------------------------------------
// Free / coalescing
// ---------------------------------------------------------------------------

// Turns an in-use block into a free one, merging it with free neighbours on both
// sides (boundary tags), then either hands the chunk back to the OS or bins it.
void Heap::release_block(BlockHeader* b) {
  std::size_t size = block_size(b);
  stats_.in_use_bytes -= size;
  // Clear INUSE on this header now: if it merges backwards below, the header is left
  // inside the predecessor's free payload and a second free must still see it as free.
  b->size_flags &= ~kInUse;

  BlockHeader* next = next_block(b);
  if (!is_in_use(next)) {
    remove_free(next);
    size += block_size(next);
  }
  if (!is_prev_in_use(b)) {
    BlockHeader* prev = prev_block(b);
    remove_free(prev);
    size += block_size(prev);
    b = prev;
  }

  // Whatever precedes the merged block is in use (otherwise it would have merged).
  set_header(b, size, kPrevInUse);
  write_footer(b);
  next_block(b)->size_flags &= ~kPrevInUse;

  if (try_release_chunk(b)) return;
  insert_free(b);
}

// If `b` (free, unbinned) now spans an entire chunk, unmap that chunk. One chunk is
// always kept so a steady alloc/free loop doesn't thrash mmap.
bool Heap::try_release_chunk(BlockHeader* b) {
  if (!owns_memory_ || !cfg_.release_empty_chunks || stats_.num_chunks <= 1) return false;
  if (block_size(next_block(b)) != 0) return false;  // next is not the epilogue

  auto* candidate = reinterpret_cast<ChunkHeader*>(reinterpret_cast<char*>(b) - kChunkHeaderSize);
  ChunkHeader** link = &chunks_;
  while (*link != nullptr && *link != candidate) link = &(*link)->next;
  if (*link == nullptr) return false;  // b is not the first block of a chunk

  *link = candidate->next;
  --stats_.num_chunks;
  const std::size_t len = candidate->size;
  note_footprint_change(-static_cast<std::ptrdiff_t>(len));
  os::unmap_pages(candidate, len);
  return true;
}

void Heap::deallocate_unlocked(void* p) {
  if (p == nullptr) return;
  BlockHeader* b = from_payload(p);
  if (b->magic != kMagic) os::fatal("free(): invalid pointer (bad magic)");
  if (!is_in_use(b)) os::fatal("free(): double free detected");
  if (b->arena_id != id_) os::fatal("free(): pointer belongs to a different arena");

  ++stats_.free_count;
  if (is_mmapped(b)) {
    free_mmapped(b);
  } else {
    release_block(b);
  }
}

// ---------------------------------------------------------------------------
// Direct-mmap large blocks
//
//   base: [map_len 8B][....][offset 8B][header 16B][payload ...]
//                                       ^ base + offset
// ---------------------------------------------------------------------------

void* Heap::allocate_mmapped(std::size_t n, std::size_t alignment) {
  const std::size_t a = alignment < kAlign ? kAlign : alignment;
  if (n > kMaxRequest) {
    ++stats_.failed_allocs;
    return nullptr;
  }
  const std::size_t len = align_up(n + 2 * kAlign + a, os::page_size());
  if (cfg_.max_footprint != 0 && stats_.footprint + len > cfg_.max_footprint) {
    ++stats_.failed_allocs;
    return nullptr;
  }
  void* base = os::map_pages(len);
  if (base == nullptr) {
    ++stats_.failed_allocs;
    return nullptr;
  }

  const std::uintptr_t user = align_up(addr(base) + 2 * kAlign, a);
  auto* b = reinterpret_cast<BlockHeader*>(user - kHeaderSize);
  const std::size_t off = static_cast<std::size_t>(addr(b) - addr(base));
  *static_cast<std::uint64_t*>(base) = len;
  *(reinterpret_cast<std::uint64_t*>(b) - 1) = off;
  set_header(b, addr(base) + len - addr(b), kInUse | kPrevInUse | kMmapped);

  stats_.in_use_bytes += block_size(b);
  stats_.mmapped_bytes += len;
  note_footprint_change(static_cast<std::ptrdiff_t>(len));
  ++stats_.alloc_count;
  return payload(b);
}

void Heap::free_mmapped(BlockHeader* b) {
  const std::uint64_t off = *(reinterpret_cast<std::uint64_t*>(b) - 1);
  char* base = reinterpret_cast<char*>(b) - off;
  const std::size_t len = *reinterpret_cast<std::uint64_t*>(base);
  stats_.in_use_bytes -= block_size(b);
  stats_.mmapped_bytes -= len;
  note_footprint_change(-static_cast<std::ptrdiff_t>(len));
  os::unmap_pages(base, len);
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

std::size_t Heap::usable_size(const void* p) {
  if (p == nullptr) return 0;
  const BlockHeader* b = from_payload(p);
  if (b->magic != kMagic) os::fatal("usable_size(): invalid pointer");
  return block_size(b) - kHeaderSize;
}

std::uint32_t Heap::owner_of(const void* p) {
  const BlockHeader* b = from_payload(p);
  if (b->magic != kMagic) os::fatal("free(): invalid pointer (bad magic)");
  return b->arena_id;
}

bool Heap::is_mmapped_ptr(const void* p) { return p != nullptr && is_mmapped(from_payload(p)); }

HeapStats Heap::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  HeapStats s = stats_;
  s.largest_free_block = 0;
  const std::size_t top = bitmap_.find_last();
  if (top < kNumBins) {
    for (BlockHeader* b = bins_[top]; b != nullptr; b = links(b)->next) {
      if (block_size(b) > s.largest_free_block) s.largest_free_block = block_size(b);
    }
  }
  return s;
}

bool Heap::validate(std::string* err) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto fail = [err](const char* msg) {
    if (err != nullptr) *err = msg;
    return false;
  };

  std::size_t free_blocks = 0;
  std::size_t free_bytes = 0;
  std::size_t chunk_in_use = 0;
  std::size_t chunk_count = 0;

  for (const ChunkHeader* c = chunks_; c != nullptr; c = c->next) {
    ++chunk_count;
    const char* end = reinterpret_cast<const char*>(c) + c->size - kEpilogueSize;
    auto* b = reinterpret_cast<const BlockHeader*>(reinterpret_cast<const char*>(c) +
                                                   kChunkHeaderSize);
    if (!is_prev_in_use(b)) return fail("first block of a chunk lacks PREV_INUSE");

    bool prev_used = true;
    while (reinterpret_cast<const char*>(b) < end) {
      if (b->magic != kMagic) return fail("block with bad magic");
      if (b->arena_id != id_) return fail("block with foreign arena id");
      const std::size_t sz = block_size(b);
      if (sz < kMinBlock || sz % kAlign != 0) return fail("block with invalid size");
      if (reinterpret_cast<const char*>(b) + sz > end) return fail("block overruns its chunk");
      if (is_prev_in_use(b) != prev_used) return fail("PREV_INUSE flag out of sync");
      if (is_mmapped(b)) return fail("mmapped flag on a chunk block");

      if (is_in_use(b)) {
        chunk_in_use += sz;
      } else {
        if (!prev_used) return fail("two adjacent free blocks (missed coalesce)");
        if (*footer(b) != sz) return fail("free block footer mismatch");
        ++free_blocks;
        free_bytes += sz;
      }
      prev_used = is_in_use(b);
      b = next_block(b);
    }
    if (reinterpret_cast<const char*>(b) != end) return fail("chunk walk missed the epilogue");
    if (block_size(b) != 0 || !is_in_use(b)) return fail("corrupt epilogue");
    if (is_prev_in_use(b) != prev_used) return fail("epilogue PREV_INUSE out of sync");
  }
  if (chunk_count != stats_.num_chunks) return fail("chunk count mismatch");

  std::size_t binned_blocks = 0;
  std::size_t binned_bytes = 0;
  for (std::size_t i = 0; i < kNumBins; ++i) {
    if ((bins_[i] != nullptr) != bitmap_.test(i)) return fail("bitmap disagrees with bin");
    const BlockHeader* prev = nullptr;
    for (const BlockHeader* b = bins_[i]; b != nullptr; b = links(b)->next) {
      if (is_in_use(b)) return fail("in-use block on a free list");
      if (bin_index(block_size(b)) != i) return fail("block in the wrong bin");
      if (links(b)->prev != prev) return fail("broken free-list back link");
      if (++binned_blocks > free_blocks) return fail("free list has stray or cyclic entries");
      binned_bytes += block_size(b);
      prev = b;
    }
  }
  if (binned_blocks != free_blocks) return fail("free block missing from bins");
  if (binned_bytes != free_bytes || stats_.free_bytes != free_bytes) {
    return fail("free byte accounting mismatch");
  }
  if (stats_.in_use_bytes < chunk_in_use ||
      stats_.in_use_bytes - chunk_in_use > stats_.mmapped_bytes) {
    return fail("in-use byte accounting mismatch");
  }
  return true;
}

// ---------------------------------------------------------------------------
// Thread-safe public entry points
// ---------------------------------------------------------------------------

void* Heap::allocate(std::size_t n) {
  std::lock_guard<std::mutex> lock(mu_);
  return allocate_unlocked(n);
}

void* Heap::try_allocate(std::size_t n, bool* contended) {
  std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
  if (!lock.owns_lock()) {
    *contended = true;
    return nullptr;
  }
  *contended = false;
  return allocate_unlocked(n);
}

void Heap::deallocate(void* p) {
  if (p == nullptr) return;
  std::lock_guard<std::mutex> lock(mu_);
  deallocate_unlocked(p);
}

void* Heap::allocate_aligned(std::size_t alignment, std::size_t n) {
  std::lock_guard<std::mutex> lock(mu_);
  return allocate_aligned_unlocked(alignment, n);
}

void* Heap::reallocate(void* p, std::size_t n) {
  std::lock_guard<std::mutex> lock(mu_);
  return reallocate_unlocked(p, n);
}

// ---------------------------------------------------------------------------
// realloc: shrink in place, grow into a free successor, or move
// ---------------------------------------------------------------------------

void* Heap::reallocate_unlocked(void* p, std::size_t n) {
  if (p == nullptr) return allocate_unlocked(n);
  if (n == 0) {
    deallocate_unlocked(p);
    return nullptr;
  }
  if (n > kMaxRequest) {
    ++stats_.failed_allocs;
    return nullptr;
  }

  BlockHeader* b = from_payload(p);
  if (b->magic != kMagic || !is_in_use(b)) os::fatal("realloc(): invalid pointer");
  if (b->arena_id != id_) os::fatal("realloc(): pointer belongs to a different arena");

  const std::size_t cur = block_size(b);
  const std::size_t cur_usable = cur - kHeaderSize;

  if (is_mmapped(b)) {
    // Keep the mapping if the new size still uses at least half of it.
    if (n <= cur_usable && n >= cur_usable / 2) return p;
    void* q = allocate_unlocked(n);
    if (q == nullptr) return nullptr;
    std::memcpy(q, p, n < cur_usable ? n : cur_usable);
    deallocate_unlocked(p);
    return q;
  }

  const std::size_t bs = request_to_block(n);
  if (bs <= cur) {
    split_tail(b, bs);
    return p;
  }

  BlockHeader* next = next_block(b);
  if (!is_in_use(next) && cur + block_size(next) >= bs) {
    remove_free(next);
    const std::size_t total = cur + block_size(next);
    const std::uint64_t prev_flag = b->size_flags & kPrevInUse;
    set_header(b, total, kInUse | prev_flag);
    next_block(b)->size_flags |= kPrevInUse;
    stats_.in_use_bytes += total - cur;
    split_tail(b, bs);
    return p;
  }

  void* q = allocate_unlocked(n);
  if (q == nullptr) return nullptr;
  std::memcpy(q, p, cur_usable);
  deallocate_unlocked(p);
  return q;
}

// ---------------------------------------------------------------------------
// Aligned allocation: over-allocate, then free the misaligned lead and the
// unused tail back into the bins.
// ---------------------------------------------------------------------------

void* Heap::allocate_aligned_unlocked(std::size_t alignment, std::size_t n) {
  if (!is_power_of_two(alignment)) {
    ++stats_.failed_allocs;
    return nullptr;
  }
  if (alignment <= kAlign) return allocate_unlocked(n);
  if (n > kMaxRequest - alignment - 2 * kMinBlock) {
    ++stats_.failed_allocs;
    return nullptr;
  }

  const std::size_t padded = n + alignment + 2 * kMinBlock;
  if (owns_memory_ && request_to_block(padded) >= cfg_.mmap_threshold) {
    return allocate_mmapped(n, alignment);
  }

  void* p = allocate_unlocked(padded);
  if (p == nullptr) return nullptr;
  BlockHeader* b = from_payload(p);
  const std::size_t want = request_to_block(n);

  if (addr(p) % alignment == 0) {
    split_tail(b, want);
    return p;
  }

  // Leave at least kMinBlock in front so the lead can stand alone as a free block.
  const std::uintptr_t aligned = align_up(addr(p) + kMinBlock, alignment);
  auto* nb = reinterpret_cast<BlockHeader*>(aligned - kHeaderSize);
  const std::size_t lead = static_cast<std::size_t>(addr(nb) - addr(b));
  const std::size_t total = block_size(b);
  const std::uint64_t prev_flag = b->size_flags & kPrevInUse;

  set_header(nb, total - lead, kInUse);  // its predecessor (the lead) is about to be freed
  set_header(b, lead, kInUse | prev_flag);
  release_block(b);
  split_tail(nb, want);
  return payload(nb);
}

}  // namespace arena
