#include "common.h"
#include "spsc.h"

#include <atomic_queue/atomic_queue.h>
#include <rigtorp/SPSCQueue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

namespace {

using clock = std::chrono::steady_clock;
using u32   = std::uint32_t;
using u64   = std::uint64_t;

// Throughput: large ring. Latency: small (still >1 so empty/full paths stay
// real).
constexpr u32 kCapThru     = 1u << 16;
constexpr u32 kCapLat      = 8;
constexpr u32 kDefaultN    = 1'000'000;
constexpr u32 kDefaultRuns = 10;
// Steady-state warm on the same Q before the timed window.
constexpr u32 kWarmThru    = 100'000;
constexpr u32 kWarmLat     = 10'000;

struct Cfg {
    u32  n     = kDefaultN;
    u32  runs  = kDefaultRuns;
    int  cpu_p = 0;
    int  cpu_c = 2;
    bool do_tp = true;
    bool do_pp = true;
    bool quick = false;
};

// cross-core (default): distinct physical cores, e.g. 0,2
// same-core: sibling threads on one core, e.g. 0,1
[[nodiscard]]
char const *pin_topology(int cpu_p, int cpu_c) {
    auto core_id = [](int cpu) -> int {
        if (cpu < 0)
            return -1;
        std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                         "/topology/core_id");
        int           id = -1;
        if (!(in >> id))
            return -1;
        return id;
    };
    int a = core_id(cpu_p), b = core_id(cpu_c);
    if (a < 0 || b < 0)
        return "unknown";
    return a == b ? "same-core" : "cross-core";
}

void pin(int cpu) {
    if (cpu < 0)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu), &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::print(stderr, "pthread_setaffinity_np");
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]]
u64 tsc() noexcept {
    unsigned aux;
    return __rdtscp(&aux);
}

// Wall-clock / TSC over ~50ms; invariant TSC assumed on Linux x86_64.
[[nodiscard]]
double ns_per_cycle() {
    using namespace std::chrono_literals;
    auto t0 = clock::now();
    u64  c0 = tsc();
    while (clock::now() - t0 < 50ms)
        pause_spin();
    u64    c1 = tsc();
    auto   t1 = clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return ns / static_cast<double>(c1 - c0);
}

// --- adapters: push / pop busy-wait, value_type = u32 ---

template <unsigned Cap>
struct Qqu {
    qqu::spsc<u32, Cap> q;

    void push(u32 v) noexcept {
        q.push(v);
    }
    void pop(u32 &v) noexcept {
        q.pop(v);
    }
};

// atomic_queue SPSC: SIZE capacity, no minimize-contention shuffle, no
// maximize-throughput. AtomicQueue2 avoids NIL reservation on the value domain.
template <unsigned Cap>
struct Aq {
    using Q = atomic_queue::AtomicQueue2<u32, Cap, false, false, false, true>;
    Q q;

    void push(u32 v) noexcept {
        q.push(v);
    }
    void pop(u32 &v) noexcept {
        v = q.pop();
    }
};

template <unsigned Cap>
struct Rigtorp {
    rigtorp::SPSCQueue<u32> q{Cap};

    void push(u32 v) noexcept {
        q.push(v);
    }
    void pop(u32 &v) noexcept {
        for (;;) {
            if (u32 *p = q.front()) {
                v = *p;
                q.pop();
                return;
            }
            pause_spin();
        }
    }
};

struct Sync {
    std::atomic<int> ready{0};
    std::atomic<int> go{0};
    std::atomic<u64> sum{0};
};

template <class Q>
double once_throughput(Cfg const &cfg, Q &q) {
    Sync              sync;
    clock::time_point t0, t1;
    std::barrier      timed(2, [&] { t0 = clock::now(); });

    auto consumer = std::thread([&] {
        pin(cfg.cpu_c);
        sync.ready.fetch_add(1, std::memory_order_release);
        while (sync.go.load(std::memory_order_acquire) == 0)
            pause_spin();
        u32 v = 0;
        for (u32 i = 0; i < kWarmThru; ++i)
            q.pop(v);
        timed.arrive_and_wait();
        u64 s = 0;
        for (u32 i = 0; i < cfg.n; ++i) {
            q.pop(v);
            s += v;
        }
        t1 = clock::now();
        sync.sum.store(s, std::memory_order_release);
    });

    pin(cfg.cpu_p);
    while (sync.ready.load(std::memory_order_acquire) < 1)
        pause_spin();

    sync.go.store(1, std::memory_order_release);
    for (u32 i = 0; i < kWarmThru; ++i)
        q.push(0);
    timed.arrive_and_wait();
    for (u32 i = 0; i < cfg.n; ++i)
        q.push(i + 1);
    consumer.join();

    u64 expect = u64(cfg.n) * u64(cfg.n + 1) / 2;
    if (sync.sum.load(std::memory_order_acquire) != expect)
        std::println(stderr, "checksum mismatch\n"), std::exit(1);

    return std::chrono::duration<double>(t1 - t0).count();
}

struct Stats {
    double mean{}, stdev{}, min{}, max{};
};

// Sample (n-1) stdev over run metrics; stdev=0 when runs==1.
Stats stats_of(std::span<double const> xs) {
    double s = 0, s2 = 0;
    for (double x : xs) {
        s += x;
        s2 += x * x;
    }
    double n    = static_cast<double>(xs.size());
    double mean = s / n;
    double var  = xs.size() > 1 ? (s2 - s * s / n) / (n - 1) : 0;
    return {mean, std::sqrt(var), xs.front(), xs.back()};
}

void print_throughput(char const *name, std::vector<double> &mps) {
    std::ranges::sort(mps);
    auto   st     = stats_of(mps);
    auto   mid    = mps.size() / 2;
    double median = mps.size() % 2 ? mps[mid] : (mps[mid - 1] + mps[mid]) / 2;

    constexpr double m = 1e-6;
    std::println("{:30} median={:.1f}m mean={:.1f}m stdev={:.1f}m min={:.1f}m "
                 "max={:.1f}m msgs/s",
                 name, median * m, st.mean * m, st.stdev * m, st.min * m,
                 st.max * m);
}

struct Competitor {
    char const                                *name;
    std::vector<double>                        samples;
    std::function<void(std::vector<double> &)> run;
};

template <std::size_t N>
void run_rounds(std::array<Competitor, N> &competitors, u32 runs) {
    std::array<std::size_t, N> order{};
    std::ranges::iota(order, std::size_t{0});
    std::mt19937 rng(std::random_device{}());
    for (u32 round = 0; round < runs; ++round) {
        std::ranges::shuffle(order, rng);
        for (auto i : order)
            competitors[i].run(competitors[i].samples);
    }
}

// Queues are often non-movable; hold two by value (prvalue elision).
template <class Q>
struct Duo {
    Q q1{};
    Q q2{};
};

// Nearest-rank: p in [0,100], xs sorted ascending, non-empty.
[[nodiscard]]
double pct(std::span<double const> xs, double p) {
    auto i = static_cast<std::size_t>(
        std::ceil(p / 100.0 * static_cast<double>(xs.size())) - 1.0);
    if (i >= xs.size())
        i = xs.size() - 1;
    return xs[i];
}

template <class Q>
void once_ping_pong(Cfg const &cfg, Duo<Q> &d, std::span<double> out_ns,
                    double nspc) {
    Sync sync;
    auto peer = std::thread([&] {
        pin(cfg.cpu_c);
        sync.ready.fetch_add(1, std::memory_order_release);
        while (sync.go.load(std::memory_order_acquire) == 0)
            pause_spin();
        u32 v = 0;
        for (u32 i = 0; i < kWarmLat + cfg.n; ++i) {
            d.q1.pop(v);
            d.q2.push(v);
        }
    });

    pin(cfg.cpu_p);
    while (sync.ready.load(std::memory_order_acquire) < 1)
        pause_spin();

    sync.go.store(1, std::memory_order_release);
    u32 v = 0;
    for (u32 i = 0; i < kWarmLat; ++i) {
        d.q1.push(0);
        d.q2.pop(v);
    }
    for (u32 i = 0; i < cfg.n; ++i) {
        u64 t0 = tsc();
        d.q1.push(i + 1);
        d.q2.pop(v);
        out_ns[i] = static_cast<double>(tsc() - t0) * nspc;
    }
    peer.join();
}

void print_ping_pong(char const *name, std::vector<double> &samples) {
    std::ranges::sort(samples);
    double p50  = pct(samples, 50);
    double p90  = pct(samples, 90);
    double p99  = pct(samples, 99);
    double p999 = pct(samples, 99.9);
    std::println("{:30} p50={:.1f} p90={:.1f} p99={:.1f} p999={:.1f} ns/rtt",
                 name, p50, p90, p99, p999);
}

int parse_cpu_pair(char const *s, int &a, int &b) {
    int x = 0, y = 0;
    if (std::sscanf(s, "%d,%d", &x, &y) != 2)
        return -1;
    a = x;
    b = y;
    return 0;
}

void usage(char const *argv0) {
    std::println(stderr,
                 "Usage: {} [--throughput|--latency|--all] [--quick] "
                 "[--scenario cross|smt] [--cpus P,C] [-n N] [-r RUNS]\n"
                 "  --scenario cross  different physical cores (default 0,2)\n"
                 "  --scenario smt    same-core SMT siblings (default 0,1)\n"
                 "Env: QQU_N QQU_RUNS QQU_CPUS",
                 argv0);
}

Cfg parse(int argc, char **argv) {
    Cfg  c;
    bool cpus_set = false;
    if (char const *e = std::getenv("QQU_N"))
        c.n = static_cast<u32>(std::strtoul(e, nullptr, 10));
    if (char const *e = std::getenv("QQU_RUNS"))
        c.runs = static_cast<u32>(std::strtoul(e, nullptr, 10));
    if (char const *e = std::getenv("QQU_CPUS")) {
        if (parse_cpu_pair(e, c.cpu_p, c.cpu_c) == 0)
            cpus_set = true;
    }

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else if (a == "--throughput") {
            c.do_tp = true;
            c.do_pp = false;
        } else if (a == "--latency") {
            c.do_tp = false;
            c.do_pp = true;
        } else if (a == "--all") {
            c.do_tp = c.do_pp = true;
        } else if (a == "--quick") {
            c.quick = true;
        } else if (a == "--scenario" && i + 1 < argc) {
            std::string_view s = argv[++i];
            if (s == "cross") {
                if (!cpus_set) {
                    c.cpu_p = 0;
                    c.cpu_c = 2;
                }
            } else if (s == "smt") {
                if (!cpus_set) {
                    c.cpu_p = 0;
                    c.cpu_c = 1;
                }
            } else {
                usage(argv[0]);
                std::exit(2);
            }
        } else if (a == "--cpus" && i + 1 < argc) {
            if (parse_cpu_pair(argv[++i], c.cpu_p, c.cpu_c)) {
                usage(argv[0]);
                std::exit(2);
            }
            cpus_set = true;
        } else if (a == "-n" && i + 1 < argc) {
            c.n = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "-r" && i + 1 < argc) {
            c.runs = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            usage(argv[0]);
            std::exit(2);
        }
    }
    if (c.quick) {
        c.n    = std::min(c.n, 100'000u);
        c.runs = std::min(c.runs, 1u);
    }
    if (c.n == 0 || c.runs == 0) {
        std::println(stderr, "n and runs must be > 0");
        std::exit(2);
    }
    return c;
}

} // namespace

int main(int argc, char **argv) {
    Cfg cfg = parse(argc, argv);

    double nspc = 0;
    if (cfg.do_pp) {
        pin(cfg.cpu_p);
        nspc = ns_per_cycle();
    }

    char const *topo = pin_topology(cfg.cpu_p, cfg.cpu_c);
    std::println("# suite=spsc n={} runs={} cpus={},{} [{}] thr_cap={} "
                 "lat_cap={} ns/cycle={:.4f}",
                 cfg.n, cfg.runs, cfg.cpu_p, cfg.cpu_c, topo, kCapThru, kCapLat,
                 nspc);

    if (cfg.do_tp) {
        std::println("---- throughput (higher is better) ----");
        std::array competitors{
            Competitor{.name    = "qqu::spsc",
                       .samples = {},
                       .run =
                           [&](auto &xs) {
                               Qqu<kCapThru> q;
                               xs.push_back(double(cfg.n) /
                                            once_throughput(cfg, q));
                           }},
            Competitor{.name    = "rigtorp::SPSCQueue",
                       .samples = {},
                       .run =
                           [&](auto &xs) {
                               Rigtorp<kCapThru> q;
                               xs.push_back(double(cfg.n) /
                                            once_throughput(cfg, q));
                           }},
            Competitor{.name    = "atomic_queue::AtomicQueue2",
                       .samples = {},
                       .run =
                           [&](auto &xs) {
                               Aq<kCapThru> q;
                               xs.push_back(double(cfg.n) /
                                            once_throughput(cfg, q));
                           }},
        };
        run_rounds(competitors, cfg.runs);
        for (auto &competitor : competitors)
            print_throughput(competitor.name, competitor.samples);
        std::println("");
    }

    if (cfg.do_pp) {
        std::println("---- latency / ping-pong (lower is better) ----");
        auto run = [&]<class Q>(std::vector<double> &xs) {
            auto offset = xs.size();
            xs.resize(offset + cfg.n);
            Duo<Q> d;
            once_ping_pong(cfg, d, std::span{xs}.subspan(offset, cfg.n), nspc);
        };
        std::array competitors{
            Competitor{.name    = "qqu::spsc",
                       .samples = {},
                       .run =
                           [&](auto &xs) {
                               run.template operator()<Qqu<kCapLat>>(xs);
                           }},
            Competitor{.name    = "rigtorp::SPSCQueue",
                       .samples = {},
                       .run =
                           [&](auto &xs) {
                               run.template operator()<Rigtorp<kCapLat>>(xs);
                           }},
            Competitor{.name    = "atomic_queue::AtomicQueue2",
                       .samples = {},
                       .run =
                           [&](auto &xs) {
                               run.template operator()<Aq<kCapLat>>(xs);
                           }},
        };
        for (auto &competitor : competitors) {
            competitor.run(competitor.samples);
            competitor.samples.clear();
            competitor.samples.reserve(static_cast<std::size_t>(cfg.n) *
                                       cfg.runs);
        }
        run_rounds(competitors, cfg.runs);
        for (auto &competitor : competitors)
            print_ping_pong(competitor.name, competitor.samples);
        std::println("");
    }
    return 0;
}
