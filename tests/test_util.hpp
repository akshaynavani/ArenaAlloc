#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "arena/heap.hpp"

namespace arena_test {

inline std::uint8_t pattern_byte(std::uintptr_t seed, std::size_t i) {
  return static_cast<std::uint8_t>((seed * 2654435761u + i * 31u) >> 3);
}

inline void fill(void* p, std::size_t n, std::uintptr_t seed) {
  auto* b = static_cast<std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) b[i] = pattern_byte(seed, i);
}

inline bool check(const void* p, std::size_t n, std::uintptr_t seed) {
  auto* b = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    if (b[i] != pattern_byte(seed, i)) return false;
  }
  return true;
}

inline ::testing::AssertionResult valid(const arena::Heap& h) {
  std::string err;
  if (h.validate(&err)) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure() << "heap invariant violated: " << err;
}

// Small xorshift PRNG so tests are deterministic and allocation-free.
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  std::size_t range(std::size_t lo, std::size_t hi) { return lo + next() % (hi - lo + 1); }
};

}  // namespace arena_test
