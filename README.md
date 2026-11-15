# qqu

English | [中文](README.zh.md)

Bounded lock-free queues for C++23.

## Queues

| class | producers | consumers | layout |
| ----- | --------- | --------- | ------ |
| `qqu::spsc<T, N>` | 1 | 1 | ring with one state byte per slot; producer owns head, consumer owns tail |
| `qqu::mpsc<T, N>` | N | 1 | Vyukov bounded ring; one sequence number per cell, CAS on head |
| `qqu::mpmc<T, N>` | N | M | same cells as `mpsc`, each padded to 64 bytes; CAS on head and tail |

## Benchmarks

v1.1.0, AMD Ryzen 5 PRO 6650U (6C/12T, 15 W), NixOS 26.05, clang 22.1.8 + libc++,
isolated cores, capacity 1024, 1M messages per producer, 10 runs. The harness sets the `performance` profile and disables CPU boost. Raw data, summaries, the full payload
matrix and the plots are in [docs/benchmarks](docs/benchmarks).

Throughput, Mops/s (mean ± 95% CI over 10 runs):

| workload         |         qqu | atomic_queue |    rigtorp |
| ---------------- | ----------: | -----------: | ---------: |
| spsc u32 (1P/1C) | 169.0 ± 5.1 |  176.5 ± 6.6 | 58.2 ± 1.3 |
| spsc u64 (1P/1C) | 142.7 ± 2.0 |  148.9 ± 2.6 | 68.3 ± 3.9 |
| mpsc u32 (2P/1C) |  22.6 ± 0.3 |   15.3 ± 0.0 | 25.0 ± 0.5 |
| mpsc u32 (3P/1C) |  29.9 ± 0.0 |   15.2 ± 0.0 | 35.1 ± 0.2 |
| mpsc u32 (4P/1C) |  35.9 ± 0.1 |   16.0 ± 0.0 | 36.8 ± 0.1 |
| mpmc u32 (1P/2C) |  18.1 ± 0.3 |   10.7 ± 0.0 | 17.9 ± 0.4 |
| mpmc u32 (2P/2C) |  18.9 ± 0.5 |    9.0 ± 0.0 | 18.5 ± 0.5 |

Latency, ns (mean ± 95% CI over 10 runs; round trip):

| workload             |         qqu | atomic_queue |     rigtorp |
| -------------------- | ----------: | -----------: | ----------: |
| spsc u32 p50 (1P/1C) | 130.2 ± 0.0 |  170.3 ± 0.0 | 220.4 ± 0.0 |
| spsc u32 p99 (1P/1C) | 186.4 ± 9.1 |  191.4 ± 2.3 | 250.5 ± 0.0 |
| mpsc u32 p50 (2P/1C) | 210.4 ± 0.0 |  240.5 ± 0.0 | 200.4 ± 0.0 |
| mpsc u32 p99 (2P/1C) | 278.5 ± 3.0 |  350.7 ± 0.0 | 289.5 ± 2.3 |
| mpmc u32 p50 (2P/2C) | 220.4 ± 0.0 |  320.6 ± 0.0 | 220.4 ± 0.0 |
| mpmc u32 p99 (2P/2C) | 285.5 ± 5.1 |  510.9 ± 0.0 | 278.5 ± 3.0 |

`± 0.0` means every run landed on the same value; the latency timer resolves about
10 ns, so differences below that are not meaningful.

What the numbers say:

- `spsc` throughput is 4% behind `atomic_queue` on u32/u64 and 2.1-2.9x
  `rigtorp::SPSCQueue`. Its p50 is 24% lower than `atomic_queue` and 32-41% lower
  than `rigtorp::SPSCQueue` on u32/u64/p16. On 64-byte payloads it is 18% behind
  `atomic_queue` on throughput; its p50 is 21% lower and its p99 4% higher.
- `mpsc` is 1.2-2.2x `atomic_queue` on throughput. On u32/u64/p16 it is 9-17% behind
  `rigtorp::MPMCQueue` at 2-3 producers and within 5% at 4; on p64 it is within 3%.
  On u32/u64 its p99 is 20-40% lower than `atomic_queue`; against
  `rigtorp::MPMCQueue` it is 4-6% lower at 2 producers and 14-30% lower at 3-4. On
  p64 its p50 is 36-45% higher than `rigtorp::MPMCQueue`. rigtorp is a general MPMC
  queue, so this is not a like-for-like win.
- `mpmc` is within 4% of `rigtorp::MPMCQueue` on throughput and 1.5-2.1x
  `atomic_queue`. Its p99 is 21-44% lower than `atomic_queue`; against
  `rigtorp::MPMCQueue` it is 13-14% higher at 1 producer and 1-5% higher at 2.

Padding each `mpmc` cell to 64 bytes raised its multi-consumer throughput by 31-50% on
4- to 16-byte payloads; padding `mpsc` cells lost throughput at 1 producer and at 3-4
producers with capacity 64, so `mpsc` is unpadded. Tables and method are in
[docs/benchmarks/cell_padding.md](docs/benchmarks/cell_padding.md).

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
reference queues), and a C++23 compiler with `std::print`: GCC 14+ or clang 21+ with
libc++ (`std::ranges::iota`). The Python harness needs Python 3.12+ and [uv](https://docs.astral.sh/uv/).

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

For clang, install `clang-22` and `libc++-22-dev` (Ubuntu 24.04 needs the
[apt.llvm.org](https://apt.llvm.org) repository) and pass
`-DCMAKE_CXX_COMPILER=clang++-22`; CMake adds `-stdlib=libc++` for Clang.

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

## Limitations

- Linux x86-64 only; the spin loop uses `_mm_pause`.
- Capacity is fixed at compile time; there is no dynamic or resizable variant.
- `T` must be trivially copyable and default constructible;
  there is no support for non-trivial element types.
- `mpsc` cells are not padded, so adjacent cells can share a cache line. `mpmc` cells
  are padded to 64 bytes: cell memory is 4x larger for u32/u64, and at capacity 1024
  1P/1C throughput on 4- to 16-byte payloads is 43-57% lower than unpadded.
- Latency is a two-core round trip and includes the hand-off cost. Its timer resolves
  about 10 ns; do not read differences below that.
- All measurements are from a single 15 W laptop CPU, without SMT sharing between
  producer and consumer. Do not compare these numbers with server results.

## License

MIT
