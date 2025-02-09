// Fragmentation / footprint benchmark.
//
// Replays one embedded-style trace against one allocator and samples the OS
// footprint every N operations. The reported overhead is footprint-weighted:
//   overhead = sum(footprint - live_requested) / sum(footprint)
// so moments when the heap is nearly empty don't dominate the average. Run each allocator in its own process (see run_benchmarks.sh)
// so the system allocator's footprint isn't distorted by anything else.
//
//   arena_frag_bench --alloc system|arena [--workload all|sensor|mixed|sawtooth|frames]
//                    [--ops 400000] [--seed 1] [--chunk-kb 64] [--sample-every 500] [--csv]
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "allocators.hpp"
#include "workloads.hpp"

namespace {

struct Options {
  std::string alloc = "arena";
  std::string workload = "all";
  std::size_t ops = 400000;
  std::uint64_t seed = 1;
  std::size_t chunk_kb = 64;
  std::size_t sample_every = 500;
  bool csv = false;
};

[[noreturn]] void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s --alloc system|arena [--workload all|sensor|mixed|sawtooth|frames]\n"
               "          [--ops N] [--seed S] [--chunk-kb K] [--sample-every N] [--csv]\n",
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
    else if (a == "--workload") o.workload = value();
    else if (a == "--ops") o.ops = std::strtoull(value(), nullptr, 10);
    else if (a == "--seed") o.seed = std::strtoull(value(), nullptr, 10);
    else if (a == "--chunk-kb") o.chunk_kb = std::strtoull(value(), nullptr, 10);
    else if (a == "--sample-every") o.sample_every = std::strtoull(value(), nullptr, 10);
    else if (a == "--csv") o.csv = true;
    else usage(argv[0]);
  }
  if (o.sample_every == 0) o.sample_every = 1;
  return o;
}

struct Result {
  std::string workload;
  std::string alloc;
  std::size_t ops = 0;
  double seconds = 0;
  std::size_t peak_live = 0;
  std::size_t peak_footprint = 0;
  double mean_overhead = 0;  // sum(footprint - live) / sum(footprint) over samples
  double peak_ratio = 0;     // peak_footprint / peak_live
  std::size_t failures = 0;
  double arena_ext_frag = -1;  // final-state external fragmentation (arena only)
};

Result run(bench::Allocator& alloc, const bench::Trace& trace, const Options& opt) {
  std::vector<void*> slots(trace.num_slots, nullptr);
  std::vector<std::uint32_t> sizes(trace.num_slots, 0);
  const std::size_t base = alloc.footprint();

  Result r;
  r.workload = trace.name;
  r.alloc = alloc.name();
  r.ops = trace.ops.size();

  std::size_t live = 0;
  double wasted_sum = 0;
  double footprint_sum = 0;

  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < trace.ops.size(); ++i) {
    const bench::Op& op = trace.ops[i];
    if (op.size != 0) {
      auto* p = static_cast<unsigned char*>(alloc.alloc(op.size));
      if (p == nullptr) {
        ++r.failures;
        slots[op.slot] = nullptr;
        sizes[op.slot] = 0;
        continue;
      }
      p[0] = 1;  // touch both ends so the pages are really committed
      p[op.size - 1] = 1;
      slots[op.slot] = p;
      sizes[op.slot] = op.size;
      live += op.size;
    } else {
      alloc.release(slots[op.slot]);
      live -= sizes[op.slot];
      slots[op.slot] = nullptr;
      sizes[op.slot] = 0;
    }

    if (i % opt.sample_every == 0 && live > 0) {
      const std::size_t raw = alloc.footprint();
      const std::size_t fp = raw > base ? raw - base : 0;
      if (fp >= live) {
        wasted_sum += static_cast<double>(fp - live);
        footprint_sum += static_cast<double>(fp);
      }
      r.peak_footprint = std::max(r.peak_footprint, fp);
      r.peak_live = std::max(r.peak_live, live);
    }
  }
  const auto t1 = std::chrono::steady_clock::now();

  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.mean_overhead = footprint_sum > 0 ? wasted_sum / footprint_sum : 0.0;
  r.peak_ratio = r.peak_live ? static_cast<double>(r.peak_footprint) / r.peak_live : 0.0;
  if (auto* a = dynamic_cast<bench::ArenaHeapAllocator*>(&alloc)) {
    r.arena_ext_frag = a->heap().stats().external_fragmentation();
  }
  return r;
}

void print(const Result& r, bool csv, bool header) {
  if (csv) {
    if (header) {
      std::printf("workload,allocator,ops,seconds,mops,peak_live_kib,peak_footprint_kib,"
                  "mean_overhead_pct,peak_ratio,failures\n");
    }
    std::printf("%s,%s,%zu,%.4f,%.3f,%zu,%zu,%.2f,%.3f,%zu\n", r.workload.c_str(),
                r.alloc.c_str(), r.ops, r.seconds, r.ops / r.seconds / 1e6, r.peak_live / 1024,
                r.peak_footprint / 1024, r.mean_overhead * 100.0, r.peak_ratio, r.failures);
    return;
  }
  std::printf("[%s / %s]\n", r.workload.c_str(), r.alloc.c_str());
  std::printf("  ops               %zu in %.3f s (%.2f Mops/s)\n", r.ops, r.seconds,
              r.ops / r.seconds / 1e6);
  std::printf("  peak live         %zu KiB\n", r.peak_live / 1024);
  std::printf("  peak footprint    %zu KiB  (%.3fx live)\n", r.peak_footprint / 1024,
              r.peak_ratio);
  std::printf("  overhead          %.2f %%  (footprint-weighted, 1 - live/footprint)\n",
              r.mean_overhead * 100.0);
  if (r.arena_ext_frag >= 0) {
    std::printf("  ext. frag (end)   %.2f %%\n", r.arena_ext_frag * 100.0);
  }
  if (r.failures) std::printf("  failed allocs     %zu\n", r.failures);
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);

  std::vector<std::string> workloads;
  if (opt.workload == "all") {
    workloads = bench::workload_names();
  } else {
    workloads.push_back(opt.workload);
  }

  // Generate every trace up front, so trace memory sits in the baseline footprint.
  std::vector<bench::Trace> traces;
  for (const auto& w : workloads) traces.push_back(bench::make_trace(w, opt.seed, opt.ops));

  arena::HeapConfig cfg;
  cfg.chunk_size = opt.chunk_kb * 1024;

  bool header = true;
  for (const auto& t : traces) {
    // A fresh allocator instance per workload (for "system" this is the same libc heap).
    auto alloc = bench::make_allocator(opt.alloc, cfg, 1);
    print(run(*alloc, t, opt), opt.csv, header);
    header = false;
  }
  return 0;
}
