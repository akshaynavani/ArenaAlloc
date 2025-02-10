#!/usr/bin/env bash
# Runs the fragmentation benchmark for every workload with each allocator in a
# separate process, then prints a comparison table plus a short throughput sweep.
#
#   ./bench/run_benchmarks.sh [build-dir] [ops]
set -euo pipefail

BUILD_DIR="${1:-build}"
OPS="${2:-400000}"
FRAG="$BUILD_DIR/arena_frag_bench"
TPUT="$BUILD_DIR/arena_throughput_bench"

for bin in "$FRAG" "$TPUT"; do
  if [[ ! -x "$bin" ]]; then
    echo "missing $bin - build first: cmake -S . -B $BUILD_DIR && cmake --build $BUILD_DIR -j" >&2
    exit 1
  fi
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== Fragmentation (ops per workload: $OPS) =="
for wl in sensor mixed sawtooth frames; do
  for alloc in system arena; do
    "$FRAG" --alloc "$alloc" --workload "$wl" --ops "$OPS" --csv | tail -n 1 >> "$TMP/frag.csv"
  done
done

# Columns: workload,allocator,ops,seconds,mops,peak_live_kib,peak_footprint_kib,
#          mean_overhead_pct,peak_ratio,failures
awk -F, '
  { key=$1; ovh[key","$2]=$8; peak[key","$2]=$7; mops[key","$2]=$5; if (!(key in seen)) order[++n]=key; seen[key]=1 }
  END {
    printf "%-10s | %14s %14s | %12s %12s | %10s %10s | %s\n",
      "workload", "sys overhead%", "arena ovh%", "sys peak KiB", "arena peak", "sys Mops", "arena Mops",
      "overhead reduction"
    # Walk workloads in the order they ran; for-in order in awk is unspecified.
    for (i = 1; i <= n; i++) {
      k = order[i]
      s = ovh[k",system"]; a = ovh[k",arena"]
      red = (s > 0) ? (s - a) / s * 100 : 0
      printf "%-10s | %14.2f %14.2f | %12d %12d | %10.2f %10.2f | %+.1f%%\n",
        k, s, a, peak[k",system"], peak[k",arena"], mops[k",system"], mops[k",arena"], red
    }
  }' "$TMP/frag.csv"

echo
echo "== Throughput (mixed workload) =="
for mode in local handoff; do
  for threads in 1 2 4 8; do
    for alloc in system arena-pool; do
      "$TPUT" --alloc "$alloc" --mode "$mode" --threads "$threads" --ops 200000 | head -n 1
    done
  done
done
