#if !defined(__linux__) || !defined(__x86_64__)
#error "qqu targets Linux only"
#endif

#include "spsc.h"

#include <atomic_queue/atomic_queue.h>
#include <rigtorp/SPSCQueue.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <print>
#include <string_view>
#include <thread>

#include <pthread.h>
#include <sched.h>

namespace {

using clock = std::chrono::steady_clock;
using u32   = std::uint32_t;
using u64   = std::uint64_t;

// Throughput: large ring. Latency: small (still >1 so empty/full paths stay real).
constexpr u32 kCapThru     = 1u << 16;
constexpr u32 kCapLat      = 8;
constexpr u32 kDefaultN    = 1'000'000;
constexpr u32 kDefaultRuns = 5;

struct Cfg {
    u32  n     = kDefaultN;
    u32  runs  = kDefaultRuns;
    int  cpu_p = 0;
    int  cpu_c = 1;
    bool do_tp = true;
    bool do_pp = true;
    bool quick = false;
};

void pin(int cpu) {
    if (cpu < 0)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu), &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

void pause_spin() noexcept {
    __builtin_ia32_pause();
}

// --- adapters: push / pop busy-wait, value_type = u32 ---

struct QquThru {
    qqu::spsc<u32, kCapThru> q;

    void push(u32 v) noexcept {
        q.push(v);
    }
    void pop(u32 &v) noexcept {
        q.pop(v);
    }
};

struct QquLat {
    qqu::spsc<u32, kCapLat> q;

    void push(u32 v) noexcept {
        q.push(v);
    }
    void pop(u32 &v) noexcept {
        q.pop(v);
    }
};

// atomic_queue SPSC: SIZE capacity, no minimize-contention shuffle, no maximize-throughput.
// AtomicQueue2 avoids NIL reservation on the value domain.
template <unsigned Cap>
struct Aq {
    using Q = atomic_queue::AtomicQueue2<u32, Cap, false, false, false, true>;
    Q q;

    void push(u32 v) noexcept {
        while (!q.try_push(v))
            pause_spin();
    }
    void pop(u32 &v) noexcept {
        while (!q.try_pop(v))
            pause_spin();
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
    Sync sync;
    auto consumer = std::thread([&] {
        pin(cfg.cpu_c);
        sync.ready.fetch_add(1, std::memory_order_release);
        while (sync.go.load(std::memory_order_acquire) == 0)
            pause_spin();
        u64 s = 0;
        u32 v = 0;
        for (u32 i = 0; i < cfg.n; ++i) {
            q.pop(v);
            s += v;
        }
        sync.sum.store(s, std::memory_order_release);
    });

    pin(cfg.cpu_p);
    while (sync.ready.load(std::memory_order_acquire) < 1)
        pause_spin();

    auto t0 = clock::now();
    sync.go.store(1, std::memory_order_release);
    for (u32 i = 0; i < cfg.n; ++i)
        q.push(i + 1);
    consumer.join();
    auto t1 = clock::now();

    u64 expect = u64(cfg.n) * u64(cfg.n + 1) / 2;
    if (sync.sum.load(std::memory_order_acquire) != expect)
        std::println(stderr, "checksum mismatch\n"), std::exit(1);

    return std::chrono::duration<double>(t1 - t0).count();
}

template <class MakeQ>
void bench_throughput(char const *name, Cfg const &cfg, MakeQ make) {
    double best = std::numeric_limits<double>::infinity();
    for (u32 r = 0; r < cfg.runs; ++r) {
        auto q = make();
        best   = std::min(best, once_throughput(cfg, q));
    }
    double mps = double(cfg.n) / best;
    std::println("{:40} {:.0f} msgs/s", name, mps);
}

// Queues are often non-movable; hold two by value (prvalue elision).
template <class Q>
struct Duo {
    Q q1{};
    Q q2{};
};

template <class Q>
double once_ping_pong(Cfg const &cfg, Duo<Q> &d) {
    Sync sync;
    auto peer = std::thread([&] {
        pin(cfg.cpu_c);
        sync.ready.fetch_add(1, std::memory_order_release);
        while (sync.go.load(std::memory_order_acquire) == 0)
            pause_spin();
        u32 v = 0;
        for (u32 i = 0; i < cfg.n; ++i) {
            d.q1.pop(v);
            d.q2.push(v);
        }
    });

    pin(cfg.cpu_p);
    while (sync.ready.load(std::memory_order_acquire) < 1)
        pause_spin();

    auto t0 = clock::now();
    sync.go.store(1, std::memory_order_release);
    u32 v = 0;
    for (u32 i = 0; i < cfg.n; ++i) {
        d.q1.push(i + 1);
        d.q2.pop(v);
    }
    peer.join();
    auto t1 = clock::now();
    (void)v;
    return std::chrono::duration<double>(t1 - t0).count();
}

template <class Q>
void bench_ping_pong(char const *name, Cfg const &cfg) {
    double best = std::numeric_limits<double>::infinity();
    for (u32 r = 0; r < cfg.runs; ++r) {
        Duo<Q> d{};
        best = std::min(best, once_ping_pong(cfg, d));
    }
    double rtt_ns = best / double(cfg.n) * 1e9;
    std::println("{:40} {:.1f} ns/round-trip", name, rtt_ns);
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
                 "Usage: {} [--throughput|--latency|--all] [--quick] [--cpus P,C] [-n N] [-r RUNS]\nEnv: QQU_N "
                 "QQU_RUNS QQU_CPUS",
                 argv0);
}

Cfg parse(int argc, char **argv) {
    Cfg c;
    if (char const *e = std::getenv("QQU_N"))
        c.n = static_cast<u32>(std::strtoul(e, nullptr, 10));
    if (char const *e = std::getenv("QQU_RUNS"))
        c.runs = static_cast<u32>(std::strtoul(e, nullptr, 10));
    if (char const *e = std::getenv("QQU_CPUS"))
        (void)parse_cpu_pair(e, c.cpu_p, c.cpu_c);

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
        } else if (a == "--cpus" && i + 1 < argc) {
            if (parse_cpu_pair(argv[++i], c.cpu_p, c.cpu_c)) {
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

} // namespace

int main(int argc, char **argv) {
    Cfg cfg = parse(argc, argv);

    std::println("# suite=spsc n={} runs={} cpus={},{} thr_cap={} lat_cap={}", cfg.n, cfg.runs, cfg.cpu_p, cfg.cpu_c,
                 kCapThru, kCapLat);

    if (cfg.do_tp) {
        std::println("---- throughput (higher is better) ----");
        bench_throughput("qqu::spsc", cfg, [] { return QquThru{}; });
        bench_throughput("rigtorp::SPSCQueue", cfg, [] { return Rigtorp<kCapThru>{}; });
        bench_throughput("atomic_queue::AtomicQueue2/SPSC", cfg, [] { return Aq<kCapThru>{}; });
        std::println("");
    }

    if (cfg.do_pp) {
        std::println("---- latency / ping-pong (lower is better) ----");
        bench_ping_pong<QquLat>("qqu::spsc", cfg);
        bench_ping_pong<Rigtorp<kCapLat>>("rigtorp::SPSCQueue", cfg);
        bench_ping_pong<Aq<kCapLat>>("atomic_queue::AtomicQueue2/SPSC", cfg);
        std::println("* unbounded/block-based; not capacity-fair vs rings");
        std::println("");
    }
    return 0;
}
