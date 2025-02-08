#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "arena/arena_pool.hpp"
#include "test_util.hpp"

using namespace arena;
using namespace arena_test;

namespace {

::testing::AssertionResult pool_valid(const ArenaPool& pool) {
  std::string err;
  if (pool.validate(&err)) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure() << "pool invariant violated: " << err;
}

PoolConfig small_pool(std::size_t arenas) {
  PoolConfig cfg;
  cfg.num_arenas = arenas;
  cfg.heap.chunk_size = 256 * 1024;
  return cfg;
}

}  // namespace

TEST(Pool, RespectsArenaCountAndCap) {
  ArenaPool four(small_pool(4));
  EXPECT_EQ(four.num_arenas(), 4u);
  PoolConfig huge = small_pool(1000);
  ArenaPool capped(huge);
  EXPECT_EQ(capped.num_arenas(), ArenaPool::kMaxArenas);
}

TEST(Pool, CallocZeroesMemoryAndDetectsOverflow) {
  ArenaPool pool(small_pool(2));
  // Dirty a block, free it, then calloc the same size class.
  void* dirty = pool.allocate(256);
  std::memset(dirty, 0xAB, 256);
  pool.deallocate(dirty);
  auto* z = static_cast<unsigned char*>(pool.allocate_zeroed(16, 16));
  ASSERT_NE(z, nullptr);
  for (int i = 0; i < 256; ++i) EXPECT_EQ(z[i], 0);
  pool.deallocate(z);
  EXPECT_EQ(pool.allocate_zeroed(SIZE_MAX / 2, 4), nullptr);
}

TEST(Pool, ConcurrentAllocFreeStress) {
  ArenaPool pool(small_pool(4));
  constexpr int kThreads = 8;
  constexpr int kOps = 20000;
  std::atomic<int> corrupt{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      Rng rng(1000 + t);
      std::vector<std::pair<void*, std::size_t>> live;
      for (int op = 0; op < kOps; ++op) {
        if (live.empty() || rng.next() % 100 < 55) {
          std::size_t n = rng.range(1, 2048);
          void* p = pool.allocate(n);
          if (p == nullptr) {
            corrupt.fetch_add(1);
            continue;
          }
          fill(p, n, reinterpret_cast<std::uintptr_t>(p));
          live.emplace_back(p, n);
        } else {
          std::size_t i = rng.next() % live.size();
          if (!check(live[i].first, live[i].second,
                     reinterpret_cast<std::uintptr_t>(live[i].first))) {
            corrupt.fetch_add(1);
          }
          pool.deallocate(live[i].first);
          live[i] = live.back();
          live.pop_back();
        }
      }
      for (auto& l : live) pool.deallocate(l.first);
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_EQ(corrupt.load(), 0);
  EXPECT_TRUE(pool_valid(pool));
  HeapStats s = pool.stats();
  EXPECT_EQ(s.in_use_bytes, 0u);
  EXPECT_EQ(s.alloc_count, s.free_count);
}

TEST(Pool, CrossThreadFreeRoutesToOwningArena) {
  ArenaPool pool(small_pool(4));
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::pair<void*, std::size_t>> queue;
  bool done = false;
  std::atomic<int> corrupt{0};
  constexpr int kProducers = 3;
  constexpr int kItemsEach = 15000;

  std::vector<std::thread> producers;
  for (int t = 0; t < kProducers; ++t) {
    producers.emplace_back([&, t] {
      Rng rng(77 + t);
      for (int i = 0; i < kItemsEach; ++i) {
        std::size_t n = rng.range(8, 1500);
        void* p = pool.allocate(n);
        fill(p, n, n);
        {
          std::lock_guard<std::mutex> lk(mu);
          queue.emplace_back(p, n);
        }
        cv.notify_one();
      }
    });
  }

  std::vector<std::thread> consumers;
  for (int t = 0; t < 2; ++t) {
    consumers.emplace_back([&] {
      while (true) {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return done || !queue.empty(); });
        if (queue.empty()) return;
        auto item = queue.front();
        queue.pop_front();
        lk.unlock();
        if (!check(item.first, item.second, item.second)) corrupt.fetch_add(1);
        // Occasionally realloc on the consumer thread before freeing.
        if (item.second % 5 == 0) item.first = pool.reallocate(item.first, item.second * 2);
        pool.deallocate(item.first);
      }
    });
  }

  for (auto& p : producers) p.join();
  {
    std::lock_guard<std::mutex> lk(mu);
    done = true;
  }
  cv.notify_all();
  for (auto& c : consumers) c.join();

  EXPECT_EQ(corrupt.load(), 0);
  EXPECT_TRUE(pool_valid(pool));
  EXPECT_EQ(pool.stats().in_use_bytes, 0u);
}

TEST(PoolDeathTest, FreeingIntoTheWrongPoolAborts) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(
      {
        ArenaPool a(small_pool(1));
        ArenaPool b(small_pool(1));
        void* p = a.allocate(32);
        b.deallocate(p);
      },
      "not allocated by this arena pool");
}
