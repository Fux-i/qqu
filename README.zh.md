# qqu

[English](README.md) | 中文

面向 C++23 的有界、固定容量无锁队列：单生产者/单消费者、多生产者/单消费者和多生产者/多消费者。仓库里还包含用来和 [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue)、[rigtorp/MPMCQueue](https://github.com/rigtorp/MPMCQueue)、[max0x7ba/atomic_queue](https://github.com/max0x7ba/atomic_queue) 对比的基准工具，运行在钉核、隔离的核上。

这是一个作品集项目。下面的数据、测量方法和限制放在一起；把它当作起点，不要当成“最快队列”的结论。

## 队列

| 类 | 生产者 | 消费者 | 结构 |
|---|---|---|---|
| `qqu::spsc<T, N>` | 1 | 1 | 环形缓冲，每个槽位一个状态字节；生产者写 head，消费者写 tail |
| `qqu::mpsc<T, N>` | N | 1 | Vyukov 有界环形队列；每个 cell 一个序号，生产者对 head 做 CAS |
| `qqu::mpmc<T, N>` | N | M | 与 `mpsc` 相同的 cell，每个填充到 64 字节；head 和 tail 都做 CAS |

`N` 是编译期的 2 的幂。元素按值存储；`T` 必须 trivially copyable、默认构造且 trivially destructible。

`spsc` 在 `N >= 4096` 时对槽位下标做位交换：低 12 位拆成两份 6 位互换，让连续槽位落在不同的 cache line。

## API

```cpp
#include <qqu/spsc.h>

qqu::spsc<std::uint64_t, 1024> queue;
queue.push(42);
std::uint64_t value{};
queue.pop(value);              // blocks while empty
bool ok = queue.try_push(1);   // false only when full
```

`push`/`pop` 在队列满/空时阻塞。`try_push`/`try_pop` 只在满/空时返回 `false`；如果另一个线程先抢到位置，会重试。`spsc` 的 `try_*` 可能短暂自旋，等待对端把槽位发布出来或清空。`size()`、`full()`、`empty()` 是近似快照。

## 构建

依赖：CMake 3.25+、Ninja、Git（CMake 构建会拉取 GoogleTest 和三个对比用的队列），以及支持 `std::print` 的 C++23 编译器：GCC 14+，或 clang 21+ 配 libc++（需要 `std::ranges::iota`）。Python harness 需要 Python 3.12+ 和 [uv](https://docs.astral.sh/uv/)。

使用 Nix devShell（clang 22 + libc++）：

```sh
nix develop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

devShell 首次进入时会按 `scripts/uv.lock` 创建 `scripts/.venv`，`qqu` 从那里运行 harness。

其他系统，以 Ubuntu 24.04 为例：

```sh
sudo apt install g++-14 ninja-build cmake git curl
curl -LsSf https://astral.sh/uv/install.sh | sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

用 clang 时安装 `clang-22` 和 `libc++-22-dev`（Ubuntu 24.04 需要添加 [apt.llvm.org](https://apt.llvm.org) 源），并传 `-DCMAKE_CXX_COMPILER=clang++-22`；CMake 会为 Clang 加 `-stdlib=libc++`。

harness 的 venv：

```sh
uv sync --project scripts
./qqu list
```

不用 uv 时：`python3 -m venv scripts/.venv && scripts/.venv/bin/pip install -e scripts`（Ubuntu 需要 `python3-venv`），然后使用 `scripts/.venv/bin/qqu`。

sanitizer 构建放在单独的目录：`-DQQU_SANITIZER=asan`、`tsan` 或 `ubsan`。

在另一个 CMake 项目里使用这些头文件：

```cmake
find_package(qqu 1.0 REQUIRED)
target_link_libraries(app PRIVATE qqu::qqu)
```

或者 `add_subdirectory(qqu)`。只有 qqu 是顶层项目时才默认构建测试和基准；可以用 `QQU_BUILD_TESTS` 或 `QQU_BUILD_BENCHMARKS` 覆盖。

## 基准

[tasks/](tasks) 里的一个 YAML 任务定义 CPU 布局、生产者数量、payload、容量、消息数、轮数和队列集合。`qqu` 负责构建目标、给线程钉核、跑矩阵，并把 CSV、文本 summary 和 SVG 图写到 `results/{raw,txt,svg}`。发布的任务每个配置跑 10 轮。

在 Nix shell 里 harness 叫 `qqu`；非 Nix 环境在 `uv sync --project scripts` 之后用 `./qqu`。下面的命令以 `qqu` 为例。

```sh
qqu list
qqu mpsc_scaling_quick
qqu summary mpsc
qqu plot mpsc
```

每一轮先预热 10 万条消息，并对所有消息做一遍 checksum，之后才计入结果。吞吐用 `steady_clock`；延迟用 `rdtscp` 测每条消息的往返。summary 报告均值、10 轮的 95% 置信区间（t 分布）和 min/max。

`performance_mode: true` 的任务会在跑之前把电源配置切到 `performance` 并关闭 CPU boost。请在交互终端里运行，sudo 会提示输入密码。无人值守时给 `powerprofilesctl` 和 `tee /sys/devices/system/cpu/cpufreq/boost` 配 NOPASSWD，或者把任务里的 `performance_mode` 改成 `false`。状态设置失败时 harness 会中止，并把 profile、governor、EPP 和 boost 写进 `meta.txt`。

## 结果

v1.1.0，AMD Ryzen 5 PRO 6650U（6C/12T，15 W），NixOS 26.05，clang 22.1.8 + libc++，隔离核，容量 1024，每生产者 100 万条消息，10 轮。harness 会把电源配置设为 `performance` 并关闭 CPU boost。原始数据、summary、完整 payload 矩阵和图在 [docs/benchmarks](docs/benchmarks)。

吞吐，Mops/s（10 轮的均值 ± 95% 置信区间）：

| 场景 | qqu | atomic_queue | rigtorp |
|---|---:|---:|---:|
| spsc u32 (1P/1C) | 169.0 ± 5.1 | 176.5 ± 6.6 | 58.2 ± 1.3 |
| spsc u64 (1P/1C) | 142.7 ± 2.0 | 148.9 ± 2.6 | 68.3 ± 3.9 |
| mpsc u32 (2P/1C) | 22.6 ± 0.3 | 15.3 ± 0.0 | 25.0 ± 0.5 |
| mpsc u32 (3P/1C) | 29.9 ± 0.0 | 15.2 ± 0.0 | 35.1 ± 0.2 |
| mpsc u32 (4P/1C) | 35.9 ± 0.1 | 16.0 ± 0.0 | 36.8 ± 0.1 |
| mpmc u32 (1P/2C) | 18.1 ± 0.3 | 10.7 ± 0.0 | 17.9 ± 0.4 |
| mpmc u32 (2P/2C) | 18.9 ± 0.5 | 9.0 ± 0.0 | 18.5 ± 0.5 |

延迟，ns（10 轮的均值 ± 95% 置信区间；往返）：

| 场景 | qqu | atomic_queue | rigtorp |
|---|---:|---:|---:|
| spsc u32 p50 (1P/1C) | 130.2 ± 0.0 | 170.3 ± 0.0 | 220.4 ± 0.0 |
| spsc u32 p99 (1P/1C) | 186.4 ± 9.1 | 191.4 ± 2.3 | 250.5 ± 0.0 |
| mpsc u32 p50 (2P/1C) | 210.4 ± 0.0 | 240.5 ± 0.0 | 200.4 ± 0.0 |
| mpsc u32 p99 (2P/1C) | 278.5 ± 3.0 | 350.7 ± 0.0 | 289.5 ± 2.3 |
| mpmc u32 p50 (2P/2C) | 220.4 ± 0.0 | 320.6 ± 0.0 | 220.4 ± 0.0 |
| mpmc u32 p99 (2P/2C) | 285.5 ± 5.1 | 510.9 ± 0.0 | 278.5 ± 3.0 |

`± 0.0` 表示 10 轮都落在同一个值上；延迟计时器的分辨率约 10 ns，低于这个的差异没有意义。

数据说明：

- `spsc` 在 u32/u64 上吞吐落后 `atomic_queue` 4%，是 `rigtorp::SPSCQueue` 的 2.1-2.9 倍。u32/u64/p16 上它的 p50 比 `atomic_queue` 低 24%，比 `rigtorp::SPSCQueue` 低 32-41%。64 字节 payload 上吞吐落后 `atomic_queue` 18%，p50 低 21%，p99 高 4%。
- `mpsc` 的吞吐是 `atomic_queue` 的 1.2-2.2 倍。u32/u64/p16 上，2-3 个生产者时落后 `rigtorp::MPMCQueue` 9-17%，4 个生产者时差距在 5% 以内；p64 差距在 3% 以内。u32/u64 的 p99 比 `atomic_queue` 低 20-40%；对 `rigtorp::MPMCQueue`，2 个生产者时低 4-6%，3-4 个生产者时低 14-30%。p64 的 p50 比 `rigtorp::MPMCQueue` 高 36-45%。rigtorp 是通用 MPMC 队列，这里不是同规格对比。
- `mpmc` 的吞吐与 `rigtorp::MPMCQueue` 相差 4% 以内，是 `atomic_queue` 的 1.5-2.1 倍。p99 比 `atomic_queue` 低 21-44%；对 `rigtorp::MPMCQueue`，1 个生产者时高 13-14%，2 个生产者时高 1-5%。

`mpmc` 的每个 cell 填充到 64 字节后，多消费者场景下 4-16 字节 payload 的吞吐提高 31-50%；`mpsc` 填充后在 1 个生产者、以及容量 64 时 3-4 个生产者下吞吐下降，所以 `mpsc` 不填充。表格和方法见 [docs/benchmarks/cell_padding.md](docs/benchmarks/cell_padding.md)。

## 限制

- 只支持 Linux x86-64；自旋用 `_mm_pause`。
- 容量在编译期固定，没有动态或可扩容版本。
- `T` 必须 trivially copyable、默认构造、trivially destructible；不支持非平凡类型。
- `mpsc` 的 cell 没有填充，相邻 cell 可能共享 cache line。`mpmc` 的 cell 填充到 64 字节：u32/u64 的 cell 内存是原来的 4 倍，容量 1024 时 1P/1C 在 4-16 字节 payload 上的吞吐比不填充低 43-57%。
- 延迟是两核往返，包含交接成本。计时器分辨率约 10 ns，低于这个的差异不要当真。
- 所有数据来自一台 15 W 笔记本 CPU，生产者和消费者之间不共享 SMT 核。不要拿去和服务器结果比较。

## License

MIT
