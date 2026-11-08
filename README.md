# qqu

English | [中文](README.zh.md)

Bounded, fixed-capacity lock-free queues for C++23: single-producer/single-consumer,
multi-producer/single-consumer and multi-producer/multi-consumer. The repository also
contains the benchmark harness used to compare them with
[rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue),
[rigtorp/MPMCQueue](https://github.com/rigtorp/MPMCQueue) and
[max0x7ba/atomic_queue](https://github.com/max0x7ba/atomic_queue) on pinned, isolated
cores.

This is a portfolio project. The measured results and their limits are below; treat
them as a starting point, not a claim of being the fastest queue available.

## Queues

| class | producers | consumers | layout |
|---|---|---|---|
| `qqu::spsc<T, N>` | 1 | 1 | ring with one state byte per slot; producer owns head, consumer owns tail |
| `qqu::mpsc<T, N>` | N | 1 | Vyukov bounded ring; one sequence number per cell, CAS on head |
| `qqu::mpmc<T, N>` | N | M | same cell layout, CAS on head and tail |

`N` is a compile-time power of two. Elements are stored by value; `T` must be
trivially copyable, default constructible and trivially destructible.

For `spsc` with `N >= 4096`, slot indices are bit-shuffled (the low 12 bits are
swapped with the next 12) so consecutive slots map to different cache lines.

## API

```cpp
#include <qqu/spsc.h>

qqu::spsc<std::uint64_t, 1024> queue;
queue.push(42);
std::uint64_t value{};
queue.pop(value);              // blocks while empty
bool ok = queue.try_push(1);   // false only when full
```

`push`/`pop` block while the queue is full/empty. `try_push`/`try_pop` return
`false` only when the queue is full/empty; they retry if another thread wins a
claim. In `spsc`, `try_*` can briefly spin while the peer has claimed a slot but not
published or cleared it. `size()`, `full()` and `empty()` are approximate snapshots.

## Build

Requirements: CMake 3.25+, Ninja, Git (the CMake build fetches GoogleTest and the
reference queues), and a C++23 compiler with `std::print`: GCC 14+ or clang 18+ with
libc++. The Python harness needs Python 3.12+ and [uv](https://docs.astral.sh/uv/).

With the Nix dev shell (clang 22 + libc++):

```sh
nix develop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The dev shell creates `scripts/.venv` from `scripts/uv.lock` on first entry; `qqu`
runs the harness from there.

On other systems, Ubuntu 24.04 as an example:

```sh
sudo apt install g++-14 ninja-build cmake git curl
curl -LsSf https://astral.sh/uv/install.sh | sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

For clang, install `clang-18` and `libc++-18-dev` and pass
`-DCMAKE_CXX_COMPILER=clang++-18`; CMake adds `-stdlib=libc++` for Clang.

The harness venv:

```sh
uv sync --project scripts
./qqu list
```

Without uv, create the venv with `python3 -m venv scripts/.venv && scripts/.venv/bin/pip
install -e scripts` (Ubuntu needs `python3-venv`), then use `scripts/.venv/bin/qqu`.

Sanitizer builds use a separate directory: `-DQQU_SANITIZER=asan`, `tsan` or `ubsan`.

To use the headers from another CMake project:

```cmake
find_package(qqu 1.0 REQUIRED)
target_link_libraries(app PRIVATE qqu::qqu)
```

or `add_subdirectory(qqu)`. Tests and benchmarks are only built when qqu is the
top-level project; set `QQU_BUILD_TESTS` or `QQU_BUILD_BENCHMARKS` to override.

## Benchmarks

One YAML task in [tasks/](tasks) defines CPU placement, producer counts, payloads,
capacities, message count, runs and the queue set. `qqu` builds the target, pins the
threads, runs the matrix and writes CSV, a text summary and SVG plots under
`results/{raw,txt,svg}`. The published tasks run 10 rounds per configuration.

Inside the Nix shell the harness is `qqu`; outside Nix use `./qqu` after
`uv sync --project scripts`. The commands below use `qqu`.

```sh
qqu list
qqu mpsc_scaling_quick
qqu summary mpsc
qqu plot mpsc
```

Every run performs a 100k-message warm-up and checks a checksum over all messages
before it is reported. Throughput is measured with `steady_clock`; latency is a
per-message round trip measured with `rdtscp`. Summaries report the mean, the 95%
confidence interval over the runs (t distribution) and the min/max range.

Tasks with `performance_mode: true` switch the power profile to `performance` and
disable CPU boost before the run. Run them from an interactive terminal: sudo asks for
your password. For unattended runs, configure NOPASSWD for `powerprofilesctl` and
`tee /sys/devices/system/cpu/cpufreq/boost`, or set `performance_mode: false` in the
task. The harness aborts if the state cannot be set, and writes the profile, governor,
EPP and boost value to `meta.txt`.

## Results

v1.0.0, AMD Ryzen 5 PRO 6650U (6C/12T, 15 W), NixOS 26.05, clang 22.1.8 + libc++,
isolated cores, capacity 1024, 1M messages per producer, 10 runs. The harness sets the
`performance` profile and disables CPU boost. Raw data, summaries, the full payload
matrix and the plots are in [docs/benchmarks](docs/benchmarks).

Throughput, Mops/s (mean ± 95% CI over 10 runs):

| workload | qqu | atomic_queue | rigtorp |
|---|---:|---:|---:|
| spsc u32 (1P/1C) | 171.2 ± 8.4 | 178.2 ± 4.2 | 53.1 ± 7.0 |
| spsc u64 (1P/1C) | 142.1 ± 4.4 | 149.9 ± 2.9 | 71.5 ± 1.1 |
| mpsc u32 (2P/1C) | 22.0 ± 0.4 | 15.4 ± 0.1 | 24.3 ± 0.4 |
| mpsc u32 (3P/1C) | 29.9 ± 0.0 | 15.2 ± 0.0 | 35.4 ± 0.1 |
| mpsc u32 (4P/1C) | 35.9 ± 0.2 | 16.0 ± 0.1 | 35.7 ± 0.1 |
| mpmc u32 (1P/2C) | 14.1 ± 0.1 | 10.9 ± 0.1 | 18.9 ± 0.2 |
| mpmc u32 (2P/2C) | 12.7 ± 0.1 | 8.9 ± 0.1 | 18.2 ± 0.6 |

Latency, ns (mean ± 95% CI over 10 runs; round trip):

| workload | qqu | atomic_queue | rigtorp |
|---|---:|---:|---:|
| spsc u32 p50 (1P/1C) | 130.2 ± 0.0 | 170.3 ± 0.0 | 219.4 ± 2.3 |
| spsc u32 p99 (1P/1C) | 185.4 ± 3.8 | 199.4 ± 2.3 | 250.5 ± 0.0 |
| mpsc u32 p50 (2P/1C) | 210.4 ± 0.0 | 221.4 ± 2.3 | 200.4 ± 0.0 |
| mpsc u32 p99 (2P/1C) | 280.5 ± 0.0 | 340.6 ± 0.0 | 292.6 ± 4.5 |
| mpmc u32 p50 (2P/2C) | 240.5 ± 0.0 | 320.6 ± 0.0 | 220.4 ± 0.0 |
| mpmc u32 p99 (2P/2C) | 380.7 ± 0.0 | 507.0 ± 3.7 | 274.5 ± 6.0 |

`± 0.0` means every run landed on the same value; the latency timer resolves about
10 ns, so differences below that are not meaningful.

What the numbers say:

- `spsc` throughput is on par with `atomic_queue` on 4- and 8-byte payloads (4-5%
  behind, inside the CI) and 2-3x `rigtorp::SPSCQueue`. Its p50 is 24% lower than
  `atomic_queue` and 37-41% lower than `rigtorp::SPSCQueue` on u32/u64/p16. On
  64-byte payloads it is 17% behind `atomic_queue` on throughput and its p50/p99 are
  49%/39% higher.
- `mpsc` is 1.4-2.2x `atomic_queue` on throughput, 9-16% behind
  `rigtorp::MPMCQueue` at 2-3 producers and level at 4. Its p99 is 18-38% lower than
  `atomic_queue`; against `rigtorp::MPMCQueue` it is level at 2 producers and 13-26%
  lower at 3-4 producers. rigtorp is a general MPMC queue, so this is not a
  like-for-like win.
- `mpmc` is 25-30% behind `rigtorp::MPMCQueue` on throughput and 29-43% ahead of
  `atomic_queue`. Its p99 is 18-25% lower than `atomic_queue` and higher than
  `rigtorp::MPMCQueue` on most payloads.

## Limitations

- Linux x86-64 only; the spin loop uses `_mm_pause`.
- Capacity is fixed at compile time; there is no dynamic or resizable variant.
- `T` must be trivially copyable, default constructible and trivially destructible;
  there is no support for non-trivial element types.
- Cells in `mpsc`/`mpmc` are not padded, so adjacent cells can share a cache line.
- Latency is a two-core round trip and includes the hand-off cost. Its timer resolves
  about 10 ns; do not read differences below that.
- The 16-byte payload's throughput CI is about 47% of its mean in this run; that row
  is kept in the CSV and summary but is not used in the tables above.
- All measurements are from a single 15 W laptop CPU, without SMT sharing between
  producer and consumer. Do not compare these numbers with server results.

## License

MIT
