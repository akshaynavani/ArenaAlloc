#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "arena/heap.hpp"
#include "test_util.hpp"

using namespace arena;
using namespace arena_test;

TEST(HeapBasic, AllocationIsAlignedAndWritable) {
  Heap h;
  for (std::size_t n : {1u, 7u, 16u, 31u, 100u, 1000u, 5000u, 100000u}) {
    void* p = h.allocate(n);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % kAlign, 0u);
    EXPECT_GE(Heap::usable_size(p), n);
    fill(p, n, n);
    EXPECT_TRUE(check(p, n, n));
    h.deallocate(p);
  }
  EXPECT_TRUE(valid(h));
}

TEST(HeapBasic, ZeroByteAllocationReturnsUniquePointer) {
  Heap h;
  void* a = h.allocate(0);
  void* b = h.allocate(0);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  h.deallocate(a);
  h.deallocate(b);
  EXPECT_TRUE(valid(h));
}

TEST(HeapBasic, LiveBlocksNeverOverlap) {
  Heap h;
  Rng rng(42);
  std::vector<std::pair<char*, std::size_t>> blocks;
  for (int i = 0; i < 2000; ++i) {
    std::size_t n = rng.range(1, 3000);
    auto* p = static_cast<char*>(h.allocate(n));
    ASSERT_NE(p, nullptr);
    blocks.emplace_back(p, Heap::usable_size(p));
  }
  std::sort(blocks.begin(), blocks.end());
  for (std::size_t i = 1; i < blocks.size(); ++i) {
    EXPECT_LE(blocks[i - 1].first + blocks[i - 1].second, blocks[i].first);
  }
  for (auto& b : blocks) h.deallocate(b.first);
  EXPECT_TRUE(valid(h));
}

TEST(HeapBasic, FreeingEverythingRestoresOneFreeBlock) {
  HeapConfig cfg;
  cfg.chunk_size = 4 << 20;
  Heap h(0, cfg);
  std::vector<void*> ptrs;
  for (int i = 0; i < 500; ++i) ptrs.push_back(h.allocate(64 + (i % 7) * 40));
  EXPECT_TRUE(valid(h));
  // Free in an interleaved order to exercise both coalescing directions.
  for (std::size_t i = 0; i < ptrs.size(); i += 2) h.deallocate(ptrs[i]);
  EXPECT_TRUE(valid(h));
  for (std::size_t i = 1; i < ptrs.size(); i += 2) h.deallocate(ptrs[i]);
  EXPECT_TRUE(valid(h));

  HeapStats s = h.stats();
  EXPECT_EQ(s.in_use_bytes, 0u);
  EXPECT_EQ(s.num_chunks, 1u);
  EXPECT_EQ(s.free_block_count, 1u);
  EXPECT_EQ(s.largest_free_block, s.free_bytes);
  EXPECT_DOUBLE_EQ(s.external_fragmentation(), 0.0);
  EXPECT_EQ(s.alloc_count, 500u);
  EXPECT_EQ(s.free_count, 500u);
}

TEST(HeapBasic, OversizedRequestFailsCleanly) {
  Heap h;
  EXPECT_EQ(h.allocate(kMaxRequest + 1), nullptr);
  EXPECT_EQ(h.stats().failed_allocs, 1u);
  EXPECT_TRUE(valid(h));
}

TEST(HeapBasic, LargeRequestsBypassBinsViaMmap) {
  HeapConfig cfg;
  cfg.mmap_threshold = 256 * 1024;
  Heap h(0, cfg);
  void* p = h.allocate(1 << 20);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(Heap::is_mmapped_ptr(p));
  EXPECT_GE(Heap::usable_size(p), std::size_t{1} << 20);
  fill(p, 1 << 20, 7);
  EXPECT_TRUE(check(p, 1 << 20, 7));

  HeapStats s = h.stats();
  EXPECT_GE(s.mmapped_bytes, std::size_t{1} << 20);
  EXPECT_EQ(s.num_chunks, 0u);
  EXPECT_TRUE(valid(h));

  h.deallocate(p);
  s = h.stats();
  EXPECT_EQ(s.mmapped_bytes, 0u);
  EXPECT_EQ(s.footprint, 0u);
  EXPECT_EQ(s.in_use_bytes, 0u);
}

TEST(HeapBasic, MaxFootprintIsEnforced) {
  HeapConfig cfg;
  cfg.chunk_size = 64 * 1024;
  cfg.max_footprint = 128 * 1024;
  Heap h(0, cfg);
  std::vector<void*> ptrs;
  while (void* p = h.allocate(4000)) ptrs.push_back(p);
  HeapStats s = h.stats();
  EXPECT_LE(s.footprint, cfg.max_footprint);
  EXPECT_GE(s.failed_allocs, 1u);
  EXPECT_GE(ptrs.size(), 20u);
  for (void* p : ptrs) h.deallocate(p);
  EXPECT_TRUE(valid(h));
}

TEST(HeapDeathTest, DoubleFreeAborts) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(
      {
        Heap h;
        void* a = h.allocate(64);
        void* guard = h.allocate(64);
        (void)guard;
        h.deallocate(a);
        h.deallocate(a);
      },
      "double free");
}

TEST(HeapDeathTest, DoubleFreeAfterBackwardMergeAborts) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(
      {
        Heap h;
        void* a = h.allocate(64);
        void* b = h.allocate(64);
        void* guard = h.allocate(64);
        (void)guard;
        h.deallocate(a);
        h.deallocate(b);  // merges back into a, leaving b's header inside the free block
        h.deallocate(b);
      },
      "double free");
}

TEST(HeapDeathTest, ForeignPointerAborts) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(
      {
        Heap h;
        alignas(16) static unsigned char junk[64] = {};
        h.deallocate(junk + 16);
      },
      "invalid pointer");
}
