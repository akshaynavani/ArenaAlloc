#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "arena/heap.hpp"
#include "test_util.hpp"

using namespace arena;
using namespace arena_test;

namespace {
constexpr std::size_t kBufSize = 64 * 1024;
alignas(16) unsigned char g_buf[kBufSize];

bool inside(const void* p, const void* base, std::size_t len) {
  auto a = reinterpret_cast<std::uintptr_t>(p);
  auto b = reinterpret_cast<std::uintptr_t>(base);
  return a >= b && a < b + len;
}
}  // namespace

TEST(FixedBuffer, ServesFromBufferUntilExhausted) {
  Heap h(g_buf, kBufSize);
  std::vector<void*> ptrs;
  while (void* p = h.allocate(512)) {
    EXPECT_TRUE(inside(p, g_buf, kBufSize));
    ptrs.push_back(p);
  }
  EXPECT_GT(ptrs.size(), 100u);
  HeapStats s = h.stats();
  EXPECT_EQ(s.num_chunks, 1u);
  EXPECT_LE(s.footprint, kBufSize);
  EXPECT_EQ(s.mmapped_bytes, 0u);
  EXPECT_GE(s.failed_allocs, 1u);
  EXPECT_TRUE(valid(h));

  for (void* p : ptrs) h.deallocate(p);
  s = h.stats();
  EXPECT_EQ(s.free_block_count, 1u);
  EXPECT_EQ(s.largest_free_block, s.footprint - 32);  // minus chunk header + epilogue
  EXPECT_TRUE(valid(h));
}

TEST(FixedBuffer, NeverFallsBackToMmap) {
  Heap h(g_buf, kBufSize);
  void* p = h.allocate(32 * 1024);
  ASSERT_NE(p, nullptr);
  EXPECT_FALSE(Heap::is_mmapped_ptr(p));
  EXPECT_TRUE(inside(p, g_buf, kBufSize));
  EXPECT_EQ(h.allocate(64 * 1024), nullptr);
  h.deallocate(p);
  EXPECT_TRUE(valid(h));
}

TEST(FixedBuffer, AcceptsMisalignedBuffer) {
  Heap h(g_buf + 3, 4096);
  void* p = h.allocate(100);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % kAlign, 0u);
  EXPECT_TRUE(inside(p, g_buf + 3, 4096));
  h.deallocate(p);
  EXPECT_TRUE(valid(h));
}

TEST(FixedBuffer, TooSmallBufferYieldsEmptyHeap) {
  Heap h(g_buf, 40);
  EXPECT_EQ(h.allocate(1), nullptr);
  EXPECT_EQ(h.stats().footprint, 0u);
  EXPECT_TRUE(valid(h));
}

TEST(FixedBuffer, AlignedAllocationStaysInsideBuffer) {
  Heap h(g_buf, kBufSize);
  void* p = h.allocate_aligned(1024, 2000);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 1024, 0u);
  EXPECT_TRUE(inside(p, g_buf, kBufSize));
  h.deallocate(p);
  EXPECT_EQ(h.stats().free_block_count, 1u);
  EXPECT_TRUE(valid(h));
}
