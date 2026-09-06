#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/../lib/common.sh"
source "$root/scripts/lib/metadata.sh"
source "$root/scripts/lib/perf.sh"
source "$root/scripts/lib/results.sh"
source "$root/scripts/lib/benchmark.sh"

results_dir=$(mktemp -d)
trap 'rm -rf "$results_dir"' EXIT
suite=all
cpus=${QQU_CPUS:-0,2,4}
scenario=cross
comment=smoke
date_stamp=20000101
time_stamp=000000
do_perf=0
full=1
only=""
bench_args=(-n 128 -r 2)

[[ $("$build/qqu_bench_mpsc" --list) == $'qqu::mpsc\natomic_queue::AtomicQueue2\nrigtorp::MPMCQueue' ]]
if "$build/qqu_bench_mpsc" --only invalid >/dev/null 2>&1; then
  echo 'unknown adapter was accepted' >&2
  exit 1
fi
if "$build/qqu_bench_spsc" --cpus 0,2,4 >/dev/null 2>&1; then
  echo 'SPSC accepted multiple producers' >&2
  exit 1
fi

qqu_run_benchmark qqu_bench_mpsc
qqu_run_benchmark qqu_bench_spsc
producer_cpus=${cpus%,*}
IFS=, read -ra producers <<<"$producer_cpus"
awk -F, -v producers="${#producers[@]}" '
  FNR == 1 { if (NF != 16) exit 1; next }
  {
    if (NF != 16 || $15 != "wait" || $16 != 128) exit 1
    if ($1 == "throughput" && $7 <= 0) exit 1
    if ($1 == "latency" && !($8 > 0 && $8 <= $9 && $9 <= $10 && $10 <= $11)) exit 1
    if ($12 == "mpsc" && $13 == producers && $14 == "fan_in_ack_rtt") mpsc++
    if ($12 == "spsc" && $13 == 1 && $14 == "ping_pong_rtt") spsc++
    cases[$12,$2,$3,$5,$1]++
  }
  END {
    if (mpsc != 144 || spsc != 144) exit 1
    for (key in cases) if (cases[key] != 2) exit 1
  }
' "$results_dir"/raw/*/*.bench.csv

only=atomic_queue::AtomicQueue2
full=0
bench_args=(--throughput --payload u64 --capacity 64 -n 1)
cpus="${cpus%%,*},${cpus##*,}"
qqu_run_benchmark qqu_bench_mpsc
awk -F, 'NR == 2 { if ($1 != "throughput" || $2 != "atomic_queue::AtomicQueue2" || $5 != 64 || $13 != 1 || $16 != 1) exit 1 } END { if (NR != 2) exit 1 }' \
  "$results_dir/raw/$date_stamp.mpsc/$time_stamp.$comment.bench.csv"

if [[ ${1:-} == --perf ]]; then
  do_perf=1
  only=""
  bench_args=(-n 128 -r 1)
  qqu_run_benchmark qqu_bench_mpsc
  awk -F, '
    NR == 1 { if (NF != 23) exit 1; next }
    { if (NF != 23 || $21 != "mpsc" || $22 != 1 || $23 != "wait" || $6 <= 0 || $7 <= 0) exit 1 }
    END { if (NR != 4) exit 1 }
  ' "$results_dir/raw/$date_stamp.mpsc/$time_stamp.$comment.perf.csv"
fi

printf 'Benchmark smoke tests passed\n'