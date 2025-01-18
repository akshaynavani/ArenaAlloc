// Segregated size classes.
//
// Small bins (exact fit): block sizes 48..1024 in 16-byte steps -> 62 bins.
// Large bins (range fit): each power of two above 1024 is split into 4 sub-bins.
// Everything past the last bin is clamped into it.
//
// The bins are ordered, so every block in bin j is larger than every block that maps
// to bin i < j. That lets find-fit take the head of any higher non-empty bin without
// scanning it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "arena/block.hpp"

namespace arena {

constexpr std::size_t kSmallMax = 1024;
constexpr std::size_t kNumSmallBins = kSmallMax / kAlign - (kMinBlock / kAlign) + 1;  // 62
constexpr std::size_t kSubBinsLog2 = 2;
constexpr std::size_t kSubBins = std::size_t{1} << kSubBinsLog2;
constexpr std::size_t kNumBins = 128;

static_assert(kNumSmallBins == 62, "unexpected small bin count");

inline unsigned floor_log2(std::size_t n) {
  return 63u - static_cast<unsigned>(__builtin_clzll(static_cast<unsigned long long>(n)));
}

inline std::size_t bin_index(std::size_t block_sz) {
  if (block_sz <= kSmallMax) {
    return block_sz / kAlign - (kMinBlock / kAlign);
  }
  const unsigned lg = floor_log2(block_sz);  // >= 10
  const std::size_t sub = (block_sz >> (lg - kSubBinsLog2)) & (kSubBins - 1);
  const std::size_t idx = kNumSmallBins + (lg - 10) * kSubBins + sub;
  return idx < kNumBins ? idx : kNumBins - 1;
}

inline bool is_small_bin(std::size_t idx) { return idx < kNumSmallBins; }

// Two-word bitmap with one bit per bin, set when the bin is non-empty.
class BinBitmap {
 public:
  void set(std::size_t i) { words_[i >> 6] |= (std::uint64_t{1} << (i & 63)); }
  void clear(std::size_t i) { words_[i >> 6] &= ~(std::uint64_t{1} << (i & 63)); }
  bool test(std::size_t i) const { return (words_[i >> 6] >> (i & 63)) & 1u; }

  // Index of the first set bit >= from, or kNumBins if none.
  std::size_t find_from(std::size_t from) const {
    if (from >= kNumBins) return kNumBins;
    std::size_t w = from >> 6;
    std::uint64_t bits = words_[w] & (~std::uint64_t{0} << (from & 63));
    while (true) {
      if (bits != 0) {
        return (w << 6) + static_cast<std::size_t>(__builtin_ctzll(bits));
      }
      if (++w >= kWords) return kNumBins;
      bits = words_[w];
    }
  }

  // Index of the highest set bit, or kNumBins if empty.
  std::size_t find_last() const {
    for (std::size_t w = kWords; w-- > 0;) {
      if (words_[w] != 0) {
        return (w << 6) + 63u - static_cast<std::size_t>(__builtin_clzll(words_[w]));
      }
    }
    return kNumBins;
  }

 private:
  static constexpr std::size_t kWords = kNumBins / 64;
  std::uint64_t words_[kWords] = {0, 0};
};

static_assert(kNumBins % 64 == 0, "bitmap assumes whole words");

}  // namespace arena
