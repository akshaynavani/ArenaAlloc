#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "arena/arena_alloc.h"

TEST(CApi, MallocFreeRoundTrip) {
  void* p = arena_malloc(123);
  ASSERT_NE(p, nullptr);
  EXPECT_GE(arena_usable_size(p), 123u);
  std::memset(p, 0x5A, 123);
  arena_free(p);
  arena_free(nullptr);  // must be a no-op
  EXPECT_EQ(arena_validate(), 1);
}

TEST(CApi, CallocReallocAligned) {
  auto* z = static_cast<unsigned char*>(arena_calloc(100, 3));
  ASSERT_NE(z, nullptr);
  for (int i = 0; i < 300; ++i) ASSERT_EQ(z[i], 0);
  z[0] = 42;
  z = static_cast<unsigned char*>(arena_realloc(z, 5000));
  ASSERT_NE(z, nullptr);
  EXPECT_EQ(z[0], 42);
  arena_free(z);

  void* a = arena_aligned_alloc(256, 77);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(a) % 256, 0u);
  arena_free(a);
  EXPECT_EQ(arena_aligned_alloc(3, 10), nullptr);
}

TEST(CApi, StatsReflectActivity) {
  arena_stats before{};
  arena_get_stats(&before);
  void* p = arena_malloc(4096);
  arena_stats during{};
  arena_get_stats(&during);
  EXPECT_GT(during.alloc_count, before.alloc_count);
  EXPECT_GE(during.in_use_bytes, before.in_use_bytes + 4096);
  EXPECT_GT(during.footprint, 0u);
  arena_free(p);
  arena_stats after{};
  arena_get_stats(&after);
  EXPECT_EQ(after.in_use_bytes, before.in_use_bytes);
  EXPECT_EQ(arena_validate(), 1);
}
