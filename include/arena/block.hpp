// Block layout and boundary-tag helpers.
//
// Every block starts with a 16-byte header:
//
//   +-------------------+----------+----------+
//   | size | flags (8B) | arena id | magic    |
//   +-------------------+----------+----------+
//
// Block sizes are multiples of 16, so the low 4 bits of `size_flags` carry flags.
// A free block also stores a doubly linked free-list node in its payload and a copy of
// its size in the last 8 bytes (the footer), which lets the following block find
// its start when coalescing backwards. An in-use block has no footer, so its payload
// runs to the end of the block. The PREV_INUSE flag tells a block whether the
// footer in front of it is valid.
#pragma once

#include <cstddef>
#include <cstdint>

namespace arena {

constexpr std::size_t kAlign = 16;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kFooterSize = 8;
// header (16) + next/prev links (16) + footer (8) = 40, rounded up to 48.
constexpr std::size_t kMinBlock = 48;
// Largest request we accept; prevents overflow when adding header/alignment.
constexpr std::size_t kMaxRequest = (std::size_t{1} << 62);

constexpr std::uint64_t kInUse = 0x1;
constexpr std::uint64_t kPrevInUse = 0x2;
constexpr std::uint64_t kMmapped = 0x4;
constexpr std::uint64_t kFlagMask = 0xF;

constexpr std::uint32_t kMagic = 0xA7E4A11Cu;

struct BlockHeader {
  std::uint64_t size_flags;
  std::uint32_t arena_id;
  std::uint32_t magic;
};
static_assert(sizeof(BlockHeader) == kHeaderSize, "header must be 16 bytes");

struct FreeLinks {
  BlockHeader* next;
  BlockHeader* prev;
};

constexpr std::size_t align_up(std::size_t n, std::size_t a) {
  return (n + a - 1) & ~(a - 1);
}

constexpr bool is_power_of_two(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

// Block size needed to satisfy a user request of `n` bytes.
constexpr std::size_t request_to_block(std::size_t n) {
  std::size_t s = align_up(n + kHeaderSize, kAlign);
  return s < kMinBlock ? kMinBlock : s;
}

inline std::size_t block_size(const BlockHeader* b) {
  return static_cast<std::size_t>(b->size_flags & ~kFlagMask);
}
inline bool is_in_use(const BlockHeader* b) { return (b->size_flags & kInUse) != 0; }
inline bool is_prev_in_use(const BlockHeader* b) { return (b->size_flags & kPrevInUse) != 0; }
inline bool is_mmapped(const BlockHeader* b) { return (b->size_flags & kMmapped) != 0; }

inline void* payload(BlockHeader* b) { return reinterpret_cast<char*>(b) + kHeaderSize; }
inline BlockHeader* from_payload(void* p) {
  return reinterpret_cast<BlockHeader*>(static_cast<char*>(p) - kHeaderSize);
}
inline const BlockHeader* from_payload(const void* p) {
  return reinterpret_cast<const BlockHeader*>(static_cast<const char*>(p) - kHeaderSize);
}

inline BlockHeader* next_block(BlockHeader* b) {
  return reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(b) + block_size(b));
}
inline const BlockHeader* next_block(const BlockHeader* b) {
  return reinterpret_cast<const BlockHeader*>(reinterpret_cast<const char*>(b) +
                                              block_size(b));
}

inline std::uint64_t* footer(BlockHeader* b) {
  return reinterpret_cast<std::uint64_t*>(reinterpret_cast<char*>(b) + block_size(b) -
                                          kFooterSize);
}
inline const std::uint64_t* footer(const BlockHeader* b) {
  return reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const char*>(b) +
                                                block_size(b) - kFooterSize);
}
inline void write_footer(BlockHeader* b) { *footer(b) = block_size(b); }

// Only valid when !is_prev_in_use(b): reads the previous block's footer.
inline BlockHeader* prev_block(BlockHeader* b) {
  std::uint64_t prev_size = *(reinterpret_cast<std::uint64_t*>(b) - 1);
  return reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(b) - prev_size);
}

inline FreeLinks* links(BlockHeader* b) { return static_cast<FreeLinks*>(payload(b)); }
inline const FreeLinks* links(const BlockHeader* b) {
  return reinterpret_cast<const FreeLinks*>(reinterpret_cast<const char*>(b) + kHeaderSize);
}

}  // namespace arena
