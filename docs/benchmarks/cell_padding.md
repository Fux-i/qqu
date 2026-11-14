# Cell padding in mpsc and mpmc

`mpsc` and `mpmc` store `{std::atomic<size_t> seq; T v;}` per cell. Without padding a
u32/u64 cell is 16 bytes, so four adjacent cells share one 64-byte cache line. This run
measures what padding each cell to a full line costs and gains.

Result: `mpmc` pads each cell to `ALIGN` (64 bytes); `mpsc` stays unpadded.

## Variants

| name | cell |
|---|---|
| unpadded | `struct cell`: 16 B for u32/u64, 24 B for p16, 72 B for p64 |
| pad64 | `struct alignas(64) cell` |
| pad128 | `struct alignas(128) cell`, to test the adjacent-line prefetcher |

The padded variants were copies of `mpsc.h`/`mpmc.h` with only the `alignas` on
`cell` changed and `_cells` aligned to `max(ALIGN, CELL)`. The shipped `mpmc` is
pad64.

## Method

- Same host and power state as the [main snapshot](README.md): Ryzen 5 PRO 6650U,
  governor/EPP `performance`, boost `0`, clang 22.1.8 + libc++, `-O3 -march=native`
  ([raw/env.txt](padding/raw/env.txt))
- built from v1.1.0 sources; the three variants run in the same binary and in
  random order every round
- capacities 64 and 1024; payloads u32, u64, p16, p64; 10 runs each
- throughput: 1,000,000 messages per producer; latency: 100,000 round trips per producer
- CPUs: mpsc producers `2,4,6,8`, consumer `10`; mpmc 1P/1C `2|8`, 2P/2C
  `2,4|8,10`, 3P/2C `2,4,6|8,10`
- tables show the median of 10 runs; latency is p50 / p99 in ns with about 10 ns
  timer resolution
- raw harness output: [padding/raw/](padding/raw/)

## Results

### Throughput, capacity 1024

| workload | payload | unpadded | pad64 | pad128 |
|---|---|---:|---:|---:|
| mpsc 1P/1C | u32 | 88.2 | 81.8 (-7%) | 74.9 (-15%) |
| mpsc 1P/1C | u64 | 88.2 | 84.2 (-4%) | 71.8 (-19%) |
| mpsc 1P/1C | p16 | 99.6 | 86.3 (-13%) | 79.4 (-20%) |
| mpsc 1P/1C | p64 | 30.6 | 26.7 (-13%) | 26.7 (-13%) |
| mpsc 2P/1C | u32 | 22.8 | 30.0 (+31%) | 27.4 (+20%) |
| mpsc 2P/1C | u64 | 22.8 | 28.1 (+24%) | 24.6 (+8%) |
| mpsc 2P/1C | p16 | 22.9 | 26.8 (+17%) | 24.9 (+9%) |
| mpsc 2P/1C | p64 | 22.5 | 22.8 (+1%) | 23.1 (+3%) |
| mpsc 3P/1C | u32 | 29.9 | 30.3 (+1%) | 30.5 (+2%) |
| mpsc 3P/1C | u64 | 29.9 | 30.1 (+1%) | 29.5 (-1%) |
| mpsc 3P/1C | p16 | 29.3 | 29.8 (+2%) | 28.8 (-2%) |
| mpsc 3P/1C | p64 | 28.2 | 29.8 (+6%) | 31.2 (+11%) |
| mpsc 4P/1C | u32 | 35.7 | 33.6 (-6%) | 34.2 (-4%) |
| mpsc 4P/1C | u64 | 36.1 | 34.2 (-5%) | 33.7 (-6%) |
| mpsc 4P/1C | p16 | 34.5 | 33.3 (-3%) | 33.8 (-2%) |
| mpsc 4P/1C | p64 | 30.6 | 30.1 (-2%) | 31.1 (+2%) |
| mpmc 1P/1C | u32 | 151.6 | 67.9 (-55%) | 67.0 (-56%) |
| mpmc 1P/1C | u64 | 156.2 | 67.8 (-57%) | 64.2 (-59%) |
| mpmc 1P/1C | p16 | 134.5 | 76.4 (-43%) | 81.9 (-39%) |
| mpmc 1P/1C | p64 | 28.2 | 27.6 (-2%) | 27.5 (-2%) |
| mpmc 2P/2C | u32 | 12.8 | 19.1 (+50%) | 18.5 (+45%) |
| mpmc 2P/2C | u64 | 13.1 | 19.6 (+50%) | 19.6 (+50%) |
| mpmc 2P/2C | p16 | 12.9 | 17.7 (+37%) | 17.2 (+33%) |
| mpmc 2P/2C | p64 | 16.3 | 18.2 (+12%) | 18.1 (+11%) |
| mpmc 3P/2C | u32 | 12.3 | 16.8 (+36%) | 17.1 (+38%) |
| mpmc 3P/2C | u64 | 12.1 | 17.0 (+40%) | 17.1 (+41%) |
| mpmc 3P/2C | p16 | 12.5 | 16.8 (+34%) | 17.1 (+36%) |
| mpmc 3P/2C | p64 | 17.1 | 19.0 (+12%) | 19.1 (+12%) |

### Latency, capacity 1024

| workload | u32 unpadded | u32 pad64 | u32 pad128 | p64 unpadded | p64 pad64 | p64 pad128 |
|---|---:|---:|---:|---:|---:|---:|
| mpsc 1P/1C | 170 / 200 | 170 / 200 | 170 / 200 | 210 / 271 | 170 / 230 | 170 / 210 |
| mpsc 2P/1C | 210 / 281 | 210 / 271 | 210 / 265 | 291 / 371 | 210 / 311 | 210 / 301 |
| mpsc 3P/1C | 210 / 311 | 210 / 281 | 200 / 281 | 291 / 381 | 210 / 291 | 210 / 291 |
| mpsc 4P/1C | 220 / 311 | 210 / 301 | 210 / 296 | 311 / 391 | 210 / 311 | 220 / 311 |
| mpmc 1P/1C | 170 / 220 | 170 / 210 | 170 / 210 | 220 / 271 | 170 / 240 | 180 / 210 |
| mpmc 2P/2C | 240 / 381 | 200 / 271 | 200 / 271 | 271 / 371 | 200 / 301 | 200 / 291 |
| mpmc 3P/2C | 260 / 411 | 220 / 311 | 210 / 291 | 311 / 411 | 220 / 331 | 210 / 311 |

### Throughput, capacity 64

| workload | payload | unpadded | pad64 | pad128 |
|---|---|---:|---:|---:|
| mpsc 1P/1C | u32 | 87.5 | 25.7 (-71%) | 25.5 (-71%) |
| mpsc 1P/1C | u64 | 87.2 | 25.7 (-71%) | 25.5 (-71%) |
| mpsc 1P/1C | p16 | 61.8 | 25.7 (-58%) | 25.5 (-59%) |
| mpsc 1P/1C | p64 | 25.6 | 25.5 (-0%) | 25.5 (-0%) |
| mpsc 2P/1C | u32 | 22.6 | 25.5 (+13%) | 25.0 (+10%) |
| mpsc 2P/1C | u64 | 21.9 | 23.5 (+7%) | 22.6 (+3%) |
| mpsc 2P/1C | p16 | 23.3 | 25.5 (+10%) | 26.8 (+15%) |
| mpsc 2P/1C | p64 | 23.8 | 25.0 (+5%) | 24.9 (+5%) |
| mpsc 3P/1C | u32 | 29.9 | 23.7 (-21%) | 25.3 (-16%) |
| mpsc 3P/1C | u64 | 29.9 | 23.7 (-21%) | 25.3 (-15%) |
| mpsc 3P/1C | p16 | 29.9 | 30.3 (+1%) | 25.3 (-16%) |
| mpsc 3P/1C | p64 | 24.4 | 25.5 (+5%) | 25.5 (+4%) |
| mpsc 4P/1C | u32 | 35.9 | 23.4 (-35%) | 24.2 (-33%) |
| mpsc 4P/1C | u64 | 35.9 | 22.9 (-36%) | 24.2 (-33%) |
| mpsc 4P/1C | p16 | 34.7 | 23.4 (-33%) | 24.2 (-30%) |
| mpsc 4P/1C | p64 | 22.9 | 24.2 (+6%) | 24.2 (+6%) |
| mpmc 1P/1C | u32 | 155.7 | 144.0 (-7%) | 143.9 (-8%) |
| mpmc 1P/1C | u64 | 150.2 | 143.4 (-5%) | 142.9 (-5%) |
| mpmc 1P/1C | p16 | 159.9 | 148.4 (-7%) | 139.6 (-13%) |
| mpmc 1P/1C | p64 | 43.4 | 47.5 (+10%) | 47.7 (+10%) |
| mpmc 2P/2C | u32 | 13.0 | 17.0 (+31%) | 17.2 (+32%) |
| mpmc 2P/2C | u64 | 13.5 | 19.2 (+43%) | 19.4 (+44%) |
| mpmc 2P/2C | p16 | 12.9 | 18.5 (+43%) | 18.6 (+45%) |
| mpmc 2P/2C | p64 | 18.1 | 19.0 (+5%) | 18.6 (+3%) |
| mpmc 3P/2C | u32 | 12.1 | 16.0 (+32%) | 16.5 (+36%) |
| mpmc 3P/2C | u64 | 12.2 | 16.4 (+35%) | 16.7 (+37%) |
| mpmc 3P/2C | p16 | 12.0 | 15.9 (+33%) | 16.9 (+41%) |
| mpmc 3P/2C | p64 | 15.8 | 16.0 (+1%) | 16.4 (+3%) |

### Latency, capacity 64

| workload | u32 unpadded | u32 pad64 | u32 pad128 | p64 unpadded | p64 pad64 | p64 pad128 |
|---|---:|---:|---:|---:|---:|---:|
| mpsc 1P/1C | 170 / 200 | 170 / 230 | 170 / 200 | 210 / 250 | 170 / 210 | 170 / 210 |
| mpsc 2P/1C | 220 / 260 | 220 / 281 | 200 / 260 | 301 / 371 | 210 / 316 | 210 / 321 |
| mpsc 3P/1C | 220 / 291 | 210 / 260 | 210 / 260 | 291 / 381 | 205 / 281 | 210 / 281 |
| mpsc 4P/1C | 220 / 311 | 210 / 281 | 210 / 265 | 311 / 391 | 220 / 291 | 220 / 291 |
| mpmc 1P/1C | 170 / 200 | 170 / 220 | 170 / 200 | 220 / 260 | 170 / 230 | 170 / 210 |
| mpmc 2P/2C | 230 / 371 | 200 / 281 | 200 / 271 | 281 / 381 | 240 / 321 | 240 / 311 |
| mpmc 3P/2C | 250 / 401 | 210 / 291 | 210 / 291 | 311 / 411 | 230 / 341 | 220 / 321 |

## Notes

- One producer and one consumer: unpadded is faster because the consumer pulls four
  u32/u64 cells per cache-line transfer. With capacity 64 the padded `mpsc` variants and
  the unpadded p64 (72 B cell) stop at about 25.5 Mops/s, which is one line transfer
  per message on this machine. At capacity 1024, padding costs `mpsc` 4-13% and `mpmc`
  43-57% on u32/u64/p16.
- `mpsc` with several producers: all producers contend on `_head`, so cell layout is
  a smaller cost. pad64 gains 17-31% at 2 producers (u32/u64/p16, capacity 1024), is
  within 2% at 3 and loses 3-6% at 4 on the same payloads. At capacity 64 it loses 21-36% at 3-4
  producers on u32/u64. This loss was not profiled.
- `mpmc` with two consumers: consumers on different cores pop adjacent cells, and
  producers write the next cells, so unpadded cells cause false sharing. pad64 gains
  31-50% on u32/u64/p16 and 1-12% on p64, whose 72 B cell already spans two lines.
  p99 drops from 381-411 ns to 271-311 ns on u32 at capacity 1024.
- pad128 is at most 7% faster than pad64 and up to 17% slower, so the adjacent-line
  prefetcher gives no gain here.
- p64 p50 drops by about 40-100 ns with padding in every `mpsc` and `mpmc` row.

## Decision

- `mpmc`: `struct alignas(ALIGN) cell`. It gains in every multi-consumer case
  measured; the 1P/1C loss is the case `spsc` covers. Cell memory grows 4x for
  u32/u64 (64 KiB at capacity 1024).
- `mpsc`: unpadded. Padding gains only at 2 producers and loses at 1 producer and at
  3-4 producers with capacity 64.
