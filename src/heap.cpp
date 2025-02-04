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

}  // namespace arena
