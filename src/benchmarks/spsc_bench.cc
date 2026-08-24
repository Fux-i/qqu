#include "spsc.h"

#include <atomic_queue/atomic_queue.h>
#include <rigtorp/SPSCQueue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
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

template <std::size_t Bytes>
struct Payload {
    std::array<u64, Bytes / sizeof(u64)> words{};
};

using Payload16 = Payload<16>;
using Payload64 = Payload<64>;

constexpr u32 kDefaultN    = 1'000'000;
constexpr u32 kDefaultRuns = 15;
// Steady-state warm on the same Q before the timed window.
constexpr u32 kWarmThru    = 100'000;
constexpr u32 kWarmLat     = 10'000;
constexpr u32 kTscSamples  = 100'001;

struct Cfg {
    u32  n     = kDefaultN;
    u32  runs  = kDefaultRuns;
    int  cpu_p = 6;
    int  cpu_c = 8;
    bool do_tp = true;
    bool do_pp = true;
    bool quick = true;
    u32  cap   = 0;

    char const *only    = nullptr;
    char const *payload = nullptr;
};

bool want_queue(char const *only, char const *name) {
    if (!only || !*only)
        return true;
    std::string_view o = only, n = name;
    if (o == n)
        return true;
    if (o == "qqu" && n.starts_with("qqu"))
        return true;
    if (o == "rigtorp" && n.starts_with("rigtorp"))
        return true;
    if ((o == "aq" || o == "atomic_queue") && n.starts_with("atomic_queue"))
        return true;
    return false;
}

// cross-core (default): distinct physical cores, e.g. 6,8
// same-core: sibling threads on one core, e.g. 6,7
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
u64 tsc_start() noexcept {
    _mm_lfence();
    return __rdtsc();
}

[[nodiscard]]
u64 tsc_end() noexcept {
    unsigned aux;
    u64      tsc = __rdtscp(&aux);
    _mm_lfence();
    return tsc;
}

// Wall-clock / TSC over ~50ms; invariant TSC assumed on Linux x86_64.
[[nodiscard]]
double ns_per_tsc_tick() {
    using namespace std::chrono_literals;
    auto t0 = clock::now();
    u64  c0 = tsc_start();
    while (clock::now() - t0 < 50ms)
        ;
    u64    c1 = tsc_end();
    auto   t1 = clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return ns / static_cast<double>(c1 - c0);
}

template <class T>
T value(u32 i) noexcept {
    if constexpr (std::integral<T>)
        return static_cast<T>(i);
    else {
        T v;
        v.words[0] = i;
        return v;
    }
}

template <class T>
u64 scalar(T const &v) noexcept {
    if constexpr (std::integral<T>)
        return v;
    else
        return v.words[0];
}

template <class T, unsigned Cap>
struct Qqu {
    using value_type = T;
    qqu::spsc<T, Cap> q;

    void push(T const &v) noexcept {
        q.push(v);
    }
    void pop(T &v) noexcept {
        q.pop(v);
    }
};

// atomic_queue SPSC: SIZE capacity, no minimize-contention shuffle, no
// maximize-throughput. AtomicQueue2 avoids NIL reservation on the value domain.
template <class T, unsigned Cap>
struct Aq {
    using value_type = T;
    using Q = atomic_queue::AtomicQueue2<T, Cap, false, false, false, true>;
    Q q;

    void push(T const &v) noexcept {
        q.push(v);
    }
    void pop(T &v) noexcept {
        v = q.pop();
    }
};

template <class T, unsigned Cap>
struct Rigtorp {
    using value_type = T;
    rigtorp::SPSCQueue<T> q{Cap};

    void push(T const &v) noexcept {
        q.push(v);
    }
    void pop(T &v) noexcept {
        for (;;) {
            if (T *p = q.front()) {
                v = *p;
                q.pop();
                return;
            }
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
    using T = typename Q::value_type;
    Sync              sync;
    clock::time_point t0, t1;
    std::barrier      timed(2, [&] { t0 = clock::now(); });

    auto consumer = std::thread([&] {
        pin(cfg.cpu_c);
        sync.ready.fetch_add(1, std::memory_order_release);
        while (sync.go.load(std::memory_order_acquire) == 0)
            ;
        T v{};
        for (u32 i = 0; i < kWarmThru; ++i)
            q.pop(v);
        timed.arrive_and_wait();
        u64 s = 0;
        for (u32 i = 0; i < cfg.n; ++i) {
            q.pop(v);
            s += scalar(v);
        }
        t1 = clock::now();
        sync.sum.store(s, std::memory_order_release);
    });

    pin(cfg.cpu_p);
    while (sync.ready.load(std::memory_order_acquire) < 1)
        ;

    sync.go.store(1, std::memory_order_release);
    for (u32 i = 0; i < kWarmThru; ++i)
        q.push(value<T>(0));
    timed.arrive_and_wait();
    for (u32 i = 0; i < cfg.n; ++i)
        q.push(value<T>(i + 1));
    consumer.join();

    u64 expect = u64(cfg.n) * u64(cfg.n + 1) / 2;
    if (sync.sum.load(std::memory_order_acquire) != expect)
        std::println(stderr, "checksum mismatch\n"), std::exit(1);

    return std::chrono::duration<double>(t1 - t0).count();
}

struct Stats {
    double mean{}, stdev{}, ci95{}, min{}, max{};
};

double t95(std::size_t df) {
    if (df == 1)
        return 12.706;
    if (df == 2)
        return 4.303;
    if (df == 3)
        return 3.182;
    if (df == 4)
        return 2.776;
    constexpr double z = 1.959964;
    double const     v = static_cast<double>(df);
    return z + (std::pow(z, 3) + z) / (4 * v) +
           (5 * std::pow(z, 5) + 16 * std::pow(z, 3) + 3 * z) / (96 * v * v) +
           (3 * std::pow(z, 7) + 19 * std::pow(z, 5) + 17 * std::pow(z, 3) -
            15 * z) /
               (384 * v * v * v);
}

// Sample (n-1) stdev over run metrics; stdev=0 when runs==1.
Stats stats_of(std::span<double const> xs) {
    double s = 0, s2 = 0;
    for (double x : xs) {
        s += x;
        s2 += x * x;
    }
    double n     = static_cast<double>(xs.size());
    double mean  = s / n;
    double var   = xs.size() > 1 ? (s2 - s * s / n) / (n - 1) : 0;
    double stdev = std::sqrt(var);
    double ci95 = xs.size() > 1 ? t95(xs.size() - 1) * stdev / std::sqrt(n) : 0;
    return {mean, stdev, ci95, xs.front(), xs.back()};
}

void print_throughput(char const *name, char const *payload, std::size_t bytes,
                      u32 capacity, std::vector<double> &mps) {
    for (std::size_t i = 0; i < mps.size(); ++i)
        std::println(
            "raw metric=throughput queue={} payload={} payload_bytes={} "
            "capacity={} run={} msgs_per_s={:.3f}",
            name, payload, bytes, capacity, i + 1, mps[i]);
    std::ranges::sort(mps);
    auto   st     = stats_of(mps);
    auto   mid    = mps.size() / 2;
    double median = mps.size() % 2 ? mps[mid] : (mps[mid - 1] + mps[mid]) / 2;

    constexpr double m = 1e-6;
    std::print("{:30} mean={:.1f}m ", name, st.mean * m);
    if (mps.size() > 1)
        std::print("95%CI=[{:.1f}m,{:.1f}m] ", (st.mean - st.ci95) * m,
                   (st.mean + st.ci95) * m);
    else
        std::print("95%CI=n/a ");
    std::println("median={:.1f}m stdev={:.1f}m min={:.1f}m max={:.1f}m msgs/s",
                 median * m, st.stdev * m, st.min * m, st.max * m);
}

template <class T>
struct Competitor {
    char const                           *name;
    std::vector<T>                        samples;
    std::function<void(std::vector<T> &)> run;
};

template <class T, std::size_t N>
void run_rounds(std::array<Competitor<T>, N> &competitors, u32 runs,
                char const *only) {
    std::array<std::size_t, N> order{};
    std::ranges::iota(order, std::size_t{0});
    std::mt19937 rng(std::random_device{}());
    for (u32 round = 0; round < runs; ++round) {
        std::ranges::shuffle(order, rng);
        for (auto i : order)
            if (want_queue(only, competitors[i].name))
                competitors[i].run(competitors[i].samples);
    }
}

template <class Q>
struct Duo {
    std::unique_ptr<Q> q1 = std::make_unique<Q>();
    std::unique_ptr<Q> q2 = std::make_unique<Q>();
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

void print_tsc_overhead(double nspt) {
    std::vector<u64> samples(kTscSamples);
    for (u64 &sample : samples) {
        u64 t0 = tsc_start();
        sample = tsc_end() - t0;
    }
    std::ranges::sort(samples);
    u64 median = samples[samples.size() / 2];
    u64 p99    = samples[(samples.size() * 99 + 99) / 100 - 1];
    std::println("TSC read overhead: median: {} ticks = {:.1f} ns; p99: {} "
                 "ticks = {:.1f} ns",
                 median, static_cast<double>(median) * nspt, p99,
                 static_cast<double>(p99) * nspt);
}

template <class Q>
void once_ping_pong(Cfg const &cfg, Duo<Q> &d, std::span<double> out_ns,
                    double nspt) {
    using T = typename Q::value_type;
    Sync sync;
    auto peer = std::thread([&] {
        pin(cfg.cpu_c);
        sync.ready.fetch_add(1, std::memory_order_release);
        while (sync.go.load(std::memory_order_acquire) == 0)
            ;
        T v{};
        for (u32 i = 0; i < kWarmLat + cfg.n; ++i) {
            d.q1->pop(v);
            d.q2->push(v);
        }
    });

    pin(cfg.cpu_p);
    while (sync.ready.load(std::memory_order_acquire) < 1)
        ;

    sync.go.store(1, std::memory_order_release);
    T v{};
    for (u32 i = 0; i < kWarmLat; ++i) {
        d.q1->push(value<T>(0));
        d.q2->pop(v);
    }
    for (u32 i = 0; i < cfg.n; ++i) {
        u64 t0 = tsc_start();
        d.q1->push(value<T>(i + 1));
        d.q2->pop(v);
        out_ns[i] = static_cast<double>(tsc_end() - t0) * nspt;
    }
    peer.join();
}

struct Latency {
    double p50, p90, p99, p999;
};

void print_ping_pong(char const *name, char const *payload, std::size_t bytes,
                     u32 capacity, std::vector<Latency> const &runs) {
    std::array<std::vector<double>, 4> values;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        auto const &run = runs[i];
        std::println("raw metric=latency queue={} payload={} payload_bytes={} "
                     "capacity={} run={} p50_ns={:.3f} p90_ns={:.3f} "
                     "p99_ns={:.3f} p999_ns={:.3f}",
                     name, payload, bytes, capacity, i + 1, run.p50, run.p90,
                     run.p99, run.p999);
        values[0].push_back(run.p50);
        values[1].push_back(run.p90);
        values[2].push_back(run.p99);
        values[3].push_back(run.p999);
    }
    std::print("{:30}", name);
    constexpr std::array labels{"p50", "p90", "p99", "p999"};
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto st = stats_of(values[i]);
        std::print(" {}_mean={:.1f} ", labels[i], st.mean);
        if (runs.size() > 1)
            std::print("95%CI=[{:.1f},{:.1f}]", st.mean - st.ci95,
                       st.mean + st.ci95);
        else
            std::print("95%CI=n/a");
    }
    std::println(" ns/rtt");
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
                 "Usage: {} [--throughput|--latency|--all] [--quick|--full] "
                 "[--scenario cross|smt] [--cpus P,C] [--only Q] "
                 "[--payload P] [--capacity N] [-n N] [-r RUNS]\n"
                 "  --scenario cross  different physical cores (default 6,8)\n"
                 "  --scenario smt    same-core SMT siblings (default 6,7)\n"
                 "  --only Q          qqu|rigtorp|aq\n"
                 "  --payload P       u32|u64|p16|p64\n"
                 "  --capacity N      64|1024|65536\n"
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
        } else if (a == "--full") {
            c.quick = false;
        } else if (a == "--scenario" && i + 1 < argc) {
            std::string_view s = argv[++i];
            if (s == "cross") {
                if (!cpus_set) {
                    c.cpu_p = 6;
                    c.cpu_c = 8;
                }
            } else if (s == "smt") {
                if (!cpus_set) {
                    c.cpu_p = 6;
                    c.cpu_c = 7;
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
        } else if (a == "--only" && i + 1 < argc) {
            c.only             = argv[++i];
            std::string_view o = c.only;
            if (o != "qqu" && o != "rigtorp" && o != "aq" &&
                o != "atomic_queue" && o != "qqu::spsc" &&
                o != "rigtorp::SPSCQueue" &&
                o != "atomic_queue::AtomicQueue2") {
                usage(argv[0]);
                std::exit(2);
            }
        } else if (a == "--payload" && i + 1 < argc) {
            c.payload          = argv[++i];
            std::string_view p = c.payload;
            if (p != "u32" && p != "u64" && p != "p16" && p != "p64") {
                usage(argv[0]);
                std::exit(2);
            }
        } else if (a == "--capacity" && i + 1 < argc) {
            c.cap = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
            if (c.cap != 64 && c.cap != 1024 && c.cap != 65536) {
                usage(argv[0]);
                std::exit(2);
            }
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

template <class T, u32 Cap>
void run_case(Cfg const &cfg, double nspt, char const *payload) {
    std::println("---- payload={} bytes={} capacity={} ----", payload,
                 sizeof(T), Cap);
    if (cfg.do_tp) {
        std::array competitors{
            Competitor<double>{"qqu::spsc",
                               {},
                               [&](auto &xs) {
                                   Qqu<T, Cap> q;
                                   xs.push_back(double(cfg.n) /
                                                once_throughput(cfg, q));
                               }},
            Competitor<double>{"rigtorp::SPSCQueue",
                               {},
                               [&](auto &xs) {
                                   Rigtorp<T, Cap> q;
                                   xs.push_back(double(cfg.n) /
                                                once_throughput(cfg, q));
                               }},
            Competitor<double>{"atomic_queue::AtomicQueue2",
                               {},
                               [&](auto &xs) {
                                   Aq<T, Cap> q;
                                   xs.push_back(double(cfg.n) /
                                                once_throughput(cfg, q));
                               }},
        };
        run_rounds(competitors, cfg.runs, cfg.only);
        for (auto &competitor : competitors)
            if (!competitor.samples.empty())
                print_throughput(competitor.name, payload, sizeof(T), Cap,
                                 competitor.samples);
    }

    if (cfg.do_pp) {
        auto run = [&]<class Q>(std::vector<Latency> &runs) {
            std::vector<double> samples(cfg.n);
            Duo<Q>              d;
            once_ping_pong(cfg, d, samples, nspt);
            std::ranges::sort(samples);
            runs.push_back({pct(samples, 50), pct(samples, 90),
                            pct(samples, 99), pct(samples, 99.9)});
        };
        std::array competitors{
            Competitor<Latency>{
                "qqu::spsc",
                {},
                [&](auto &xs) { run.template operator()<Qqu<T, Cap>>(xs); }},
            Competitor<Latency>{"rigtorp::SPSCQueue",
                                {},
                                [&](auto &xs) {
                                    run.template operator()<Rigtorp<T, Cap>>(
                                        xs);
                                }},
            Competitor<Latency>{
                "atomic_queue::AtomicQueue2",
                {},
                [&](auto &xs) { run.template operator()<Aq<T, Cap>>(xs); }},
        };
        for (auto &competitor : competitors) {
            if (!want_queue(cfg.only, competitor.name))
                continue;
            competitor.run(competitor.samples);
            competitor.samples.clear();
            competitor.samples.reserve(cfg.runs);
        }
        run_rounds(competitors, cfg.runs, cfg.only);
        for (auto &competitor : competitors)
            if (!competitor.samples.empty())
                print_ping_pong(competitor.name, payload, sizeof(T), Cap,
                                competitor.samples);
    }
}

template <class T>
void run_payload(Cfg const &cfg, double nspt, char const *payload) {
    if (cfg.cap == 64)
        run_case<T, 64>(cfg, nspt, payload);
    else if (cfg.cap == 1024)
        run_case<T, 1024>(cfg, nspt, payload);
    else if (cfg.cap == 65536)
        run_case<T, 65536>(cfg, nspt, payload);
    else if (cfg.quick)
        run_case<T, 1024>(cfg, nspt, payload);
    else {
        run_case<T, 64>(cfg, nspt, payload);
        run_case<T, 1024>(cfg, nspt, payload);
        run_case<T, 65536>(cfg, nspt, payload);
    }
}

} // namespace

int main(int argc, char **argv) {
    Cfg cfg = parse(argc, argv);

    double nspt = 0;
    if (cfg.do_pp) {
        pin(cfg.cpu_p);
        nspt = ns_per_tsc_tick();
    }

    char const *topo = pin_topology(cfg.cpu_p, cfg.cpu_c);
    char const *caps = cfg.cap     ? (cfg.cap == 64     ? "64"
                                      : cfg.cap == 1024 ? "1024"
                                                        : "65536")
                       : cfg.quick ? "1024"
                                   : "64,1024,65536";
    std::println("# suite=spsc n={} runs={} cpus={},{} [{}] capacities={} "
                 "payloads={} only={} ns/tsc_tick={:.4f}",
                 cfg.n, cfg.runs, cfg.cpu_p, cfg.cpu_c, topo, caps,
                 cfg.payload ? cfg.payload : "u32,u64,p16,p64",
                 cfg.only ? cfg.only : "all", nspt);
    if (cfg.do_pp)
        print_tsc_overhead(nspt);
    if (!cfg.payload || std::string_view(cfg.payload) == "u32")
        run_payload<u32>(cfg, nspt, "u32");
    if (!cfg.payload || std::string_view(cfg.payload) == "u64")
        run_payload<u64>(cfg, nspt, "u64");
    if (!cfg.payload || std::string_view(cfg.payload) == "p16")
        run_payload<Payload16>(cfg, nspt, "p16");
    if (!cfg.payload || std::string_view(cfg.payload) == "p64")
        run_payload<Payload64>(cfg, nspt, "p64");
    return 0;
}
