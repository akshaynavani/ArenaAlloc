#include <gtest/gtest.h>

#include <vector>

#include "arena/heap.hpp"
#include "test_util.hpp"

using namespace arena;
using namespace arena_test;

TEST(Coalesce, MergesWithBothNeighbours) {
  Heap h;
  void* a = h.allocate(200);
  void* b = h.allocate(200);
  void* c = h.allocate(200);
  ASSERT_TRUE(valid(h));
  EXPECT_EQ(h.stats().free_block_count, 1u);  // the chunk remainder

  h.deallocate(a);
  EXPECT_TRUE(valid(h));
  EXPECT_EQ(h.stats().free_block_count, 2u);  // a + remainder

  h.deallocate(c);  // merges forward into the remainder
  EXPECT_TRUE(valid(h));
  EXPECT_EQ(h.stats().free_block_count, 2u);

  h.deallocate(b);  // bridges a and c+remainder
  EXPECT_TRUE(valid(h));
  EXPECT_EQ(h.stats().free_block_count, 1u);
  EXPECT_DOUBLE_EQ(h.stats().external_fragmentation(), 0.0);
}

TEST(Coalesce, ExactSmallBinIsReusedFirst) {
  Heap h;
  void* x = h.allocate(64);
  void* guard = h.allocate(64);
  h.deallocate(x);
  void* y = h.allocate(64);
  EXPECT_EQ(x, y);
  h.deallocate(y);
  h.deallocate(guard);
  EXPECT_TRUE(valid(h));
}

TEST(Coalesce, SplitReturnsRemainderToBins) {
  Heap h;
  void* p = h.allocate(1000);
  h.deallocate(p);
  void* q = h.allocate(100);
  EXPECT_EQ(p, q);  // carved from the start of the coalesced chunk
  EXPECT_EQ(h.stats().free_block_count, 1u);
  h.deallocate(q);
  EXPECT_TRUE(valid(h));
}

TEST(Coalesce, EmptyChunksAreReturnedToTheOs) {
  HeapConfig cfg;
  cfg.chunk_size = 64 * 1024;
  Heap h(0, cfg);
  std::vector<void*> ptrs;
  for (int i = 0; i < 300; ++i) ptrs.push_back(h.allocate(1000));
  HeapStats s = h.stats();
  EXPECT_GE(s.num_chunks, 4u);
  EXPECT_TRUE(valid(h));

  for (void* p : ptrs) h.deallocate(p);
  s = h.stats();
  EXPECT_TRUE(valid(h));
  EXPECT_EQ(s.num_chunks, 1u);
  EXPECT_EQ(s.footprint, cfg.chunk_size);
  EXPECT_GE(s.peak_footprint, 4 * cfg.chunk_size);
}

TEST(Coalesce, RandomStressKeepsInvariants) {
  HeapConfig cfg;
  cfg.chunk_size = 128 * 1024;
  Heap h(0, cfg);
  Rng rng(1234);
  struct Live {
    void* p;
    std::size_t n;
  };
  std::vector<Live> live;
  for (int op = 0; op < 20000; ++op) {
    const bool do_alloc = live.empty() || (rng.next() % 100) < 55;
    if (do_alloc) {
      std::size_t n = (rng.next() % 10 == 0) ? rng.range(1025, 16384) : rng.range(1, 1024);
      void* p = h.allocate(n);
      ASSERT_NE(p, nullptr);
      fill(p, n, reinterpret_cast<std::uintptr_t>(p));
      live.push_back({p, n});
    } else {
      std::size_t i = rng.next() % live.size();
      ASSERT_TRUE(check(live[i].p, live[i].n, reinterpret_cast<std::uintptr_t>(live[i].p)))
          << "heap corrupted a live block";
      h.deallocate(live[i].p);
      live[i] = live.back();
      live.pop_back();
    }
    if (op % 1000 == 0) ASSERT_TRUE(valid(h)) << "after op " << op;
  }
  for (auto& l : live) h.deallocate(l.p);
  EXPECT_TRUE(valid(h));
  EXPECT_EQ(h.stats().in_use_bytes, 0u);
}
