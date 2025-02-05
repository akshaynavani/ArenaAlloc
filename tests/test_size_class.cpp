#include <gtest/gtest.h>

#include "arena/block.hpp"
#include "arena/size_class.hpp"

using namespace arena;

TEST(SizeClass, RequestRoundsUpToMinBlockAndAlignment) {
  EXPECT_EQ(request_to_block(0), kMinBlock);
  EXPECT_EQ(request_to_block(1), kMinBlock);
  EXPECT_EQ(request_to_block(32), kMinBlock);
  EXPECT_EQ(request_to_block(33), 64u);
  EXPECT_EQ(request_to_block(48), 64u);
  EXPECT_EQ(request_to_block(1000), 1024u);
  for (std::size_t n = 0; n < 5000; ++n) {
    const std::size_t bs = request_to_block(n);
    EXPECT_EQ(bs % kAlign, 0u);
    EXPECT_GE(bs, n + kHeaderSize);
    EXPECT_GE(bs, kMinBlock);
  }
}

TEST(SizeClass, SmallBinsAreExact) {
  EXPECT_EQ(bin_index(48), 0u);
  EXPECT_EQ(bin_index(64), 1u);
  EXPECT_EQ(bin_index(1024), kNumSmallBins - 1);
  for (std::size_t s = kMinBlock; s <= kSmallMax; s += kAlign) {
    EXPECT_TRUE(is_small_bin(bin_index(s)));
    if (s > kMinBlock) EXPECT_EQ(bin_index(s), bin_index(s - kAlign) + 1);
  }
}

TEST(SizeClass, LargeBinsAreMonotonicAndClamped) {
  EXPECT_EQ(bin_index(1040), kNumSmallBins);
  std::size_t prev = 0;
  for (std::size_t s = kMinBlock; s <= (std::size_t{1} << 26); s += (s < 8192 ? 16 : s / 64)) {
    const std::size_t idx = bin_index(s);
    EXPECT_GE(idx, prev) << "size " << s;
    EXPECT_LT(idx, kNumBins);
    prev = idx;
  }
  EXPECT_EQ(bin_index(std::size_t{1} << 40), kNumBins - 1);
}

TEST(BinBitmap, FindFromAndLast) {
  BinBitmap bm;
  EXPECT_EQ(bm.find_from(0), kNumBins);
  EXPECT_EQ(bm.find_last(), kNumBins);

  bm.set(3);
  bm.set(70);
  bm.set(127);
  EXPECT_TRUE(bm.test(70));
  EXPECT_EQ(bm.find_from(0), 3u);
  EXPECT_EQ(bm.find_from(4), 70u);
  EXPECT_EQ(bm.find_from(70), 70u);
  EXPECT_EQ(bm.find_from(71), 127u);
  EXPECT_EQ(bm.find_from(128), kNumBins);
  EXPECT_EQ(bm.find_last(), 127u);

  bm.clear(127);
  EXPECT_EQ(bm.find_from(71), kNumBins);
  EXPECT_EQ(bm.find_last(), 70u);
}
