# Benchmark results

This directory is a curated snapshot of one benchmark session: raw per-run CSVs, the
harness summaries, run metadata and five plots. The working copies under `results/`
are gitignored; this directory is the published evidence for the numbers in the
top-level README.

## Environment

| field | value |
|---|---|
| version | v1.0.0 |
| CPU | AMD Ryzen 5 PRO 6650U with Radeon Graphics (6 cores / 12 threads, 15 W) |
| kernel | 6.18.43 (NixOS 26.05) |
| isolation | `isolcpus=2-11 nohz_full=2-11 rcu_nocbs=2-11 irqaffinity=0-1` |
| power profile | `performance` (set by the harness) |
| CPU governor / EPP | `performance` / `performance` |
| CPU boost | `0` (disabled by the harness) |
| compiler | clang 22.1.8, libc++ (store path recorded in `raw/*.meta.txt`) |
| flags | Release (`-O3`), `-march=native` on benchmark targets only |

The harness writes these fields into `raw/*.meta.txt` for every producer count.

## Commands

Inside the Nix dev shell:

```sh
nix develop
qqu spsc_baseline
qqu mpsc_scaling_quick
qqu mpmc_scaling_quick
```

On other systems, after `uv sync --project scripts`:

```sh
./qqu spsc_baseline
./qqu mpsc_scaling_quick
./qqu mpmc_scaling_quick
```

## Protocol

- capacity 1024, 1,000,000 messages per producer, 10 runs per configuration
- 100,000 warm-up messages per producer before timing
- SPSC: producer 2, consumer 10; MPSC: producers 2,4,6,8, consumer 10;
  MPMC: producers 2,4, consumers 8,10
- `taskset` for the process, `pthread_setaffinity_np` for each thread
- every throughput run verifies a checksum over all messages
- throughput from `steady_clock`; latency is a per-message round trip from `rdtscp`
- summaries report mean ± 95% CI (t distribution, 10 runs, t = 2.262) and min/max

## Files

- `*.summary.txt` — harness summary per task, with mean ± CI and range
- `raw/*.csv` — one row per run (`n`, percentiles, CPU placement)
- `raw/*.meta.txt` — per producer-count metadata, including the power state
- `svg/spsc.1p.throughput.svg` — SPSC throughput across payloads
- `svg/spsc.1p.p50.svg`, `svg/spsc.1p.p99.svg` — SPSC round-trip latency
- `svg/mpsc.scaling.throughput.svg` — MPSC throughput vs producer count
- `svg/mpsc.scaling.p99.svg` — MPSC p99 vs producer count

Only the plots that show qqu's stronger configurations are included; the full matrix
is in the CSV and summary files.

## Caveats

- Latency is a two-core round trip; the timer resolves about 10 ns, so differences
  below that are not meaningful. `± 0.0` in a summary means all runs produced the
  same tick value.
- The 16-byte SPSC throughput has a 47% CI in this run (63.6-215.6 Mops/s); treat it
  as unstable.
- One machine only: a 15 W laptop CPU, single socket, no NUMA, no SMT sharing
  between producer and consumer.
- `mpsc` is compared against two MPMC implementations because no dedicated MPSC
  queue is included.
