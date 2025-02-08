#include "workloads.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace bench {

namespace {

struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  std::uint32_t range(std::uint32_t lo, std::uint32_t hi) {
    return lo + static_cast<std::uint32_t>(next() % (hi - lo + 1));
  }
  bool chance(unsigned percent) { return next() % 100 < percent; }
};

// Tracks slot allocation so generators can think in terms of "live objects".
class Builder {
 public:
  explicit Builder(std::string name) { trace_.name = std::move(name); }

  std::uint32_t alloc(std::uint32_t size) {
    std::uint32_t slot;
    if (!free_slots_.empty()) {
      slot = free_slots_.back();
      free_slots_.pop_back();
    } else {
      slot = trace_.num_slots++;
      slot_size_.push_back(0);
      live_pos_.push_back(0);
    }
    trace_.ops.push_back({slot, size});
    slot_size_[slot] = size;
    live_pos_[slot] = live_.size();
    live_.push_back(slot);
    live_bytes_ += size;
    trace_.peak_live_bytes = std::max(trace_.peak_live_bytes, live_bytes_);
    return slot;
  }

  void free(std::uint32_t slot) {
    trace_.ops.push_back({slot, 0});
    live_bytes_ -= slot_size_[slot];
    slot_size_[slot] = 0;
    // O(1) removal from the live list.
    const std::size_t pos = live_pos_[slot];
    const std::uint32_t last = live_.back();
    live_[pos] = last;
    live_pos_[last] = pos;
    live_.pop_back();
    free_slots_.push_back(slot);
  }

  void free_random(Rng& rng) {
    if (!live_.empty()) free(live_[rng.next() % live_.size()]);
  }

  std::size_t live_count() const { return live_.size(); }
  std::size_t op_count() const { return trace_.ops.size(); }

  Trace finish() {
    while (!live_.empty()) free(live_.back());
    return std::move(trace_);
  }

 private:
  Trace trace_;
  std::vector<std::uint32_t> free_slots_;
  std::vector<std::uint32_t> slot_size_;
  std::vector<std::size_t> live_pos_;
  std::vector<std::uint32_t> live_;
  std::size_t live_bytes_ = 0;
};

Trace sensor(std::uint64_t seed, std::size_t target) {
  static const std::uint32_t kPacketTypes[] = {24, 32, 40, 48, 64, 96, 128, 200, 256};
  constexpr std::size_t kWindow = 2000;
  Rng rng(seed);
  Builder b("sensor");
  while (b.op_count() < target) {
    if (b.live_count() < kWindow / 2 || (b.live_count() < kWindow && rng.chance(50))) {
      b.alloc(kPacketTypes[rng.next() % (sizeof(kPacketTypes) / sizeof(kPacketTypes[0]))]);
    } else {
      b.free_random(rng);
    }
  }
  return b.finish();
}

Trace mixed(std::uint64_t seed, std::size_t target) {
  constexpr std::size_t kTargetLive = 4000;
  Rng rng(seed);
  Builder b("mixed");
  while (b.op_count() < target) {
    const bool grow = b.live_count() < kTargetLive ? rng.chance(65) : rng.chance(45);
    if (grow || b.live_count() == 0) {
      b.alloc(rng.chance(80) ? rng.range(16, 128) : rng.range(1024, 8192));
    } else {
      b.free_random(rng);
    }
  }
  return b.finish();
}

Trace sawtooth(std::uint64_t seed, std::size_t target) {
  constexpr std::uint32_t kPerCycle = 6000;
  Rng rng(seed);
  Builder b("sawtooth");
  std::vector<std::vector<std::uint32_t>> survivors;  // per cycle
  for (std::uint32_t cycle = 0; b.op_count() < target; ++cycle) {
    // The size mix drifts between cycles so old holes don't fit new requests exactly.
    const std::uint32_t scale = 1 + cycle % 4;
    std::vector<std::uint32_t> batch;
    batch.reserve(kPerCycle);
    for (std::uint32_t i = 0; i < kPerCycle; ++i) {
      batch.push_back(b.alloc(rng.range(16, 96) * scale));
    }
    // Tear down: keep ~10% as long-lived survivors scattered through the batch.
    std::vector<std::uint32_t> keep;
    for (std::uint32_t slot : batch) {
      if (rng.chance(10)) {
        keep.push_back(slot);
      } else {
        b.free(slot);
      }
    }
    survivors.push_back(std::move(keep));
    // Survivors live for three cycles.
    if (survivors.size() > 3) {
      for (std::uint32_t slot : survivors.front()) b.free(slot);
      survivors.erase(survivors.begin());
    }
  }
  return b.finish();
}

Trace frames(std::uint64_t seed, std::size_t target) {
  constexpr std::size_t kRing = 8;
  constexpr std::size_t kMaxControl = 3000;
  Rng rng(seed);
  Builder b("frames");
  std::vector<std::uint32_t> ring;     // FIFO of live frame slots
  std::vector<std::uint32_t> control;  // live control-structure slots
  while (b.op_count() < target) {
    ring.push_back(b.alloc(rng.range(16, 64) * 1024));
    if (ring.size() > kRing) {
      b.free(ring.front());
      ring.erase(ring.begin());
    }
    // Control structures: descriptors, metadata, callbacks with random lifetimes.
    const std::uint32_t ctl = rng.range(4, 16);
    for (std::uint32_t i = 0; i < ctl; ++i) control.push_back(b.alloc(rng.range(32, 160)));
    std::uint32_t frees = rng.range(2, 16);
    if (control.size() > kMaxControl) frees += 16;
    for (std::uint32_t i = 0; i < frees && !control.empty(); ++i) {
      const std::size_t k = rng.next() % control.size();
      b.free(control[k]);
      control[k] = control.back();
      control.pop_back();
    }
  }
  return b.finish();
}

}  // namespace

const std::vector<std::string>& workload_names() {
  static const std::vector<std::string> names = {"sensor", "mixed", "sawtooth", "frames"};
  return names;
}

Trace make_trace(const std::string& workload, std::uint64_t seed, std::size_t target_ops) {
  if (workload == "sensor") return sensor(seed, target_ops);
  if (workload == "mixed") return mixed(seed, target_ops);
  if (workload == "sawtooth") return sawtooth(seed, target_ops);
  if (workload == "frames") return frames(seed, target_ops);
  throw std::invalid_argument("unknown workload: " + workload);
}

}  // namespace bench
