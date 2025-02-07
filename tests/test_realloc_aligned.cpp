#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "arena/heap.hpp"
#include "test_util.hpp"

using namespace arena;
using namespace arena_test;

TEST(Realloc, GrowsInPlaceIntoFreeSuccessor) {
  Heap h;
  void* a = h.allocate(100);
  fill(a, 100, 1);
  void* r = h.reallocate(a, 500);
  EXPECT_EQ(r, a);
  EXPECT_TRUE(check(r, 100, 1));
  EXPECT_GE(Heap::usable_size(r), 500u);
  EXPECT_TRUE(valid(h));
  h.deallocate(r);
}

TEST(Realloc, ShrinksInPlaceAndFreesTail) {
  Heap h;
  void* p = h.allocate(2000);
  void* guard = h.allocate(16);
  fill(p, 100, 2);
  const std::size_t free_before = h.stats().free_bytes;
  void* r = h.reallocate(p, 100);
  EXPECT_EQ(r, p);
  EXPECT_TRUE(check(r, 100, 2));
  EXPECT_GT(h.stats().free_bytes, free_before);
  EXPECT_TRUE(valid(h));
  h.deallocate(r);
  h.deallocate(guard);
  EXPECT_TRUE(valid(h));
}

TEST(Realloc, MovesWhenBlockedAndPreservesData) {
  Heap h;
  void* a = h.allocate(100);
  void* b = h.allocate(100);
  fill(a, 100, 3);
  void* r = h.reallocate(a, 4000);
  ASSERT_NE(r, nullptr);
  EXPECT_NE(r, a);
  EXPECT_TRUE(check(r, 100, 3));
  EXPECT_TRUE(valid(h));
  h.deallocate(r);
  h.deallocate(b);
  EXPECT_EQ(h.stats().in_use_bytes, 0u);
}

TEST(Realloc, NullAndZeroFollowCSemantics) {
  Heap h;
  void* p = h.reallocate(nullptr, 10);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(h.reallocate(p, 0), nullptr);
  EXPECT_EQ(h.stats().in_use_bytes, 0u);
  EXPECT_TRUE(valid(h));
}

TEST(Realloc, MmappedBlocksMoveWhenGrowing) {
  Heap h;
  void* p = h.allocate(1 << 20);
  ASSERT_TRUE(Heap::is_mmapped_ptr(p));
  fill(p, 1 << 20, 4);
  void* r = h.reallocate(p, 3 << 20);
  ASSERT_NE(r, nullptr);
  EXPECT_TRUE(check(r, 1 << 20, 4));
  void* same = h.reallocate(r, (3 << 20) - 100);
  EXPECT_EQ(same, r);  // still uses most of the mapping
  h.deallocate(same);
  EXPECT_EQ(h.stats().footprint, 0u);
}

TEST(Aligned, HonoursPowerOfTwoAlignments) {
  Heap h;
  std::vector<void*> ptrs;
  for (std::size_t a : {32u, 64u, 128u, 256u, 1024u, 4096u}) {
    for (std::size_t n : {1u, 100u, 3000u}) {
      void* p = h.allocate_aligned(a, n);
      ASSERT_NE(p, nullptr);
      EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % a, 0u) << "align " << a;
      EXPECT_GE(Heap::usable_size(p), n);
      fill(p, n, a + n);
      ptrs.push_back(p);
      ASSERT_TRUE(valid(h));
    }
  }
  for (void* p : ptrs) h.deallocate(p);
  EXPECT_TRUE(valid(h));
  EXPECT_EQ(h.stats().in_use_bytes, 0u);
}

TEST(Aligned, SmallAlignmentFallsBackAndBadAlignmentFails) {
  Heap h;
  void* p = h.allocate_aligned(8, 10);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % kAlign, 0u);
  h.deallocate(p);
  EXPECT_EQ(h.allocate_aligned(48, 10), nullptr);
  EXPECT_EQ(h.allocate_aligned(0, 10), nullptr);
}

TEST(Aligned, LargeAlignedRequestsUseMmap) {
  Heap h;
  void* p = h.allocate_aligned(1 << 16, 1 << 20);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(Heap::is_mmapped_ptr(p));
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % (1 << 16), 0u);
  fill(p, 1 << 20, 9);
  h.deallocate(p);
  EXPECT_EQ(h.stats().footprint, 0u);
}
