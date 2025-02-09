// Multi-threaded throughput benchmark.
//
//   local   - every thread replays its own trace (seeded per thread) against the
//             shared allocator; measures lock contention on the alloc/free fast paths.
//   handoff - threads work in producer/consumer pairs: producers allocate and push
//             blocks through a lock-free SPSC ring, and consumers free them. Every free
//             is a cross-thread free.
//
//   arena_throughput_bench --alloc system|arena-pool [--mode local|handoff]
//                          [--threads 4] [--ops 200000] [--workload mixed] [--arenas 0]
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "allocators.hpp"
#include "workloads.hpp"

namespace {

struct Options {
  std::string alloc = "arena-pool";
  std::string mode = "local";
  std::string workload = "mixed";
  std::size_t threads = 4;
  std::size_t ops = 200000;
  std::size_t arenas = 0;
};

[[noreturn]] void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s --alloc system|arena-pool [--mode local|handoff] [--threads N]\n"
               "          [--ops N] [--workload sensor|mixed|sawtooth|frames] [--arenas N]\n",
               argv0);
  std::exit(2);
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) usage(argv[0]);
      return argv[++i];
    };
    if (a == "--alloc") o.alloc = value();
    else if (a == "--mode") o.mode = value();
    else if (a == "--workload") o.workload = value();
    else if (a == "--threads") o.threads = std::strtoull(value(), nullptr, 10);
    else if (a == "--ops") o.ops = std::strtoull(value(), nullptr, 10);
    else if (a == "--arenas") o.arenas = std::strtoull(value(), nullptr, 10);
    else usage(argv[0]);
  }
  if (o.threads == 0) o.threads = 1;
  return o;
}

// Single-producer / single-consumer ring of pointers.
class SpscRing {
 public:
  void push(void* p) {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    while (h - tail_.load(std::memory_order_acquire) == kCap) std::this_thread::yield();
    buf_[h & (kCap - 1)] = p;
    head_.store(h + 1, std::memory_order_release);
  }
  void* pop() {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    while (head_.load(std::memory_order_acquire) == t) std::this_thread::yield();
    void* p = buf_[t & (kCap - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return p;
  }

 private:
  static constexpr std::size_t kCap = 4096;
  void* buf_[kCap];
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

void replay(bench::Allocator& alloc, const bench::Trace& trace) {
  std::vector<void*> slots(trace.num_slots, nullptr);
  for (const bench::Op& op : trace.ops) {
    if (op.size != 0) {
      auto* p = static_cast<unsigned char*>(alloc.alloc(op.size));
      if (p != nullptr) p[0] = 1;
      slots[op.slot] = p;
    } else {
      alloc.release(slots[op.slot]);
      slots[op.slot] = nullptr;
    }
  }
}

double run_local(bench::Allocator& alloc, const Options& opt, std::size_t* total_ops) {
  std::vector<bench::Trace> traces;
  for (std::size_t t = 0; t < opt.threads; ++t) {
    traces.push_back(bench::make_trace(opt.workload, 1000 + t, opt.ops));
    *total_ops += traces.back().ops.size();
  }
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < opt.threads; ++t) {
    threads.emplace_back([&, t] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      replay(alloc, traces[t]);
    });
  }
  const auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  for (auto& th : threads) th.join();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

double run_handoff(bench::Allocator& alloc, const Options& opt, std::size_t* total_ops) {
  const std::size_t pairs = opt.threads / 2 ? opt.threads / 2 : 1;
  std::vector<SpscRing> rings(pairs);
  // Pre-generate request sizes from the trace's allocation ops.
  std::vector<std::vector<std::uint32_t>> sizes(pairs);
  for (std::size_t p = 0; p < pairs; ++p) {
    for (const auto& op : bench::make_trace(opt.workload, 2000 + p, opt.ops).ops) {
      if (op.size != 0) sizes[p].push_back(op.size);
    }
    *total_ops += 2 * sizes[p].size();  // one alloc + one free each
  }

  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (std::size_t p = 0; p < pairs; ++p) {
    threads.emplace_back([&, p] {  // producer
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (std::uint32_t n : sizes[p]) {
        auto* b = static_cast<unsigned char*>(alloc.alloc(n));
        if (b != nullptr) b[0] = 1;
        rings[p].push(b);
      }
      rings[p].push(reinterpret_cast<void*>(std::uintptr_t{1}));  // end marker
    });
    threads.emplace_back([&, p] {  // consumer
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (;;) {
        void* b = rings[p].pop();
        if (b == reinterpret_cast<void*>(std::uintptr_t{1})) break;
        alloc.release(b);
      }
    });
  }
  const auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  for (auto& th : threads) th.join();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  auto alloc = bench::make_allocator(opt.alloc, arena::HeapConfig{}, opt.arenas);

  std::size_t total_ops = 0;
  double secs = 0;
  if (opt.mode == "local") {
    secs = run_local(*alloc, opt, &total_ops);
  } else if (opt.mode == "handoff") {
    secs = run_handoff(*alloc, opt, &total_ops);
  } else {
    usage(argv[0]);
  }

  std::printf("allocator=%s mode=%s workload=%s threads=%zu ops=%zu time=%.3fs "
              "throughput=%.2f Mops/s\n",
              alloc->name(), opt.mode.c_str(), opt.workload.c_str(), opt.threads, total_ops,
              secs, total_ops / secs / 1e6);

  if (auto* pool = dynamic_cast<bench::ArenaPoolAllocator*>(alloc.get())) {
    const arena::HeapStats s = pool->pool().stats();
    std::printf("arena-pool: arenas=%zu peak_footprint=%zu KiB in_use_after=%zu valid=%s\n",
                pool->pool().num_arenas(), s.peak_footprint / 1024, s.in_use_bytes,
                pool->pool().validate() ? "yes" : "NO");
  }
  return 0;
}
