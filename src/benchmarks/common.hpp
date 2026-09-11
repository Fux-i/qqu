#pragma once

#include <algorithm>
#include <array>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <numeric>
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

namespace qqu::benchmark {

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
constexpr u32 kWarmThru    = 100'000;
constexpr u32 kWarmLat     = 10'000;
constexpr u32 kTscSamples  = 100'001;

inline bool want_queue(char const *only, std::string_view name) {
    if (!only || !*only)
        return true;
    std::string_view list = only;
    while (!list.empty()) {
        auto comma = list.find(',');
        auto item  = list.substr(0, comma);
        if (item == name)
            return true;
        if (comma == std::string_view::npos)
            break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

inline void pin(int cpu) {
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
inline u64 tsc_start() noexcept {
    _mm_lfence();
    return __rdtsc();
}

[[nodiscard]]
inline u64 tsc_end() noexcept {
    unsigned aux;
    u64      tsc = __rdtscp(&aux);
    _mm_lfence();
    return tsc;
}

[[nodiscard]]
inline double ns_per_tsc_tick() {
    using namespace std::chrono_literals;
    auto start = clock::now();
    u64  first = tsc_start();
    while (clock::now() - start < 50ms)
        ;
    u64    last = tsc_end();
    double ns =
        std::chrono::duration<double, std::nano>(clock::now() - start).count();
    return ns / static_cast<double>(last - first);
}

template <class T>
T value(u32 index) noexcept {
    if constexpr (std::integral<T>)
        return static_cast<T>(index);
    else {
        T result;
        result.words[0] = index;
        return result;
    }
}

template <class T>
u64 scalar(T const &input) noexcept {
    if constexpr (std::integral<T>)
        return input;
    else
        return input.words[0];
}

struct Stats {
    double mean{}, stdev{}, ci95{}, min{}, max{};
};

inline double t95(std::size_t df) {
    if (df == 1)
        return 12.706;
    if (df == 2)
        return 4.303;
    if (df == 3)
        return 3.182;
    if (df == 4)
        return 2.776;
    constexpr double quantile = 1.959964;
    double const     degrees  = static_cast<double>(df);
    return quantile + (std::pow(quantile, 3) + quantile) / (4 * degrees) +
           (5 * std::pow(quantile, 5) + 16 * std::pow(quantile, 3) +
            3 * quantile) /
               (96 * degrees * degrees) +
           (3 * std::pow(quantile, 7) + 19 * std::pow(quantile, 5) +
            17 * std::pow(quantile, 3) - 15 * quantile) /
               (384 * degrees * degrees * degrees);
}

inline Stats stats_of(std::span<double const> samples) {
    double sum = 0, squares = 0;
    for (double sample : samples) {
        sum += sample;
        squares += sample * sample;
    }
    double count = static_cast<double>(samples.size());
    double mean  = sum / count;
    double variance =
        samples.size() > 1 ? (squares - sum * sum / count) / (count - 1) : 0;
    double stdev = std::sqrt(variance);
    double ci95  = samples.size() > 1
                       ? t95(samples.size() - 1) * stdev / std::sqrt(count)
                       : 0;
    return {mean, stdev, ci95, samples.front(), samples.back()};
}

inline void print_throughput(std::string_view name, char const *payload,
                             std::size_t bytes, u32 capacity,
                             std::vector<double> &mps,
                             std::string_view     context) {
    for (std::size_t index = 0; index < mps.size(); ++index)
        std::println(
            "raw metric=throughput queue={} payload={} payload_bytes={} "
            "capacity={} run={} msgs_per_s={:.3f} {}",
            name, payload, bytes, capacity, index + 1, mps[index], context);
    std::ranges::sort(mps);
    auto   stats  = stats_of(mps);
    auto   middle = mps.size() / 2;
    double median =
        mps.size() % 2 ? mps[middle] : (mps[middle - 1] + mps[middle]) / 2;

    constexpr double millionth = 1e-6;
    std::print("{:30} mean={:.1f}m ", name, stats.mean * millionth);
    if (mps.size() > 1)
        std::print("95%CI=[{:.1f}m,{:.1f}m] ",
                   (stats.mean - stats.ci95) * millionth,
                   (stats.mean + stats.ci95) * millionth);
    else
        std::print("95%CI=n/a ");
    std::println("median={:.1f}m stdev={:.1f}m min={:.1f}m max={:.1f}m msgs/s",
                 median * millionth, stats.stdev * millionth,
                 stats.min * millionth, stats.max * millionth);
}

template <class T>
struct Competitor {
    std::string_view                      name;
    std::vector<T>                        samples;
    std::function<void(std::vector<T> &)> run;
};

template <class T>
void run_rounds(std::vector<Competitor<T>> &competitors, u32 runs,
                char const *only) {
    std::vector<std::size_t> order(competitors.size());
    std::ranges::iota(order, std::size_t{0});
    std::mt19937 rng(std::random_device{}());
    for (u32 round = 0; round < runs; ++round) {
        std::ranges::shuffle(order, rng);
        for (auto index : order)
            if (want_queue(only, competitors[index].name))
                competitors[index].run(competitors[index].samples);
    }
}

[[nodiscard]]
inline double pct(std::span<double const> samples, double percentile) {
    auto index = static_cast<std::size_t>(
        std::ceil(percentile / 100.0 * static_cast<double>(samples.size())) -
        1.0);
    if (index >= samples.size())
        index = samples.size() - 1;
    return samples[index];
}

inline void print_tsc_overhead(double nspt) {
    std::vector<u64> samples(kTscSamples);
    for (u64 &sample : samples) {
        u64 start = tsc_start();
        sample    = tsc_end() - start;
    }
    std::ranges::sort(samples);
    u64 median = samples[samples.size() / 2];
    u64 p99    = samples[(samples.size() * 99 + 99) / 100 - 1];
    std::println("TSC read overhead: median: {} ticks = {:.1f} ns; p99: {} "
                 "ticks = {:.1f} ns",
                 median, static_cast<double>(median) * nspt, p99,
                 static_cast<double>(p99) * nspt);
}

struct Latency {
    double p50, p90, p99, p999;
};

inline void print_latency(std::string_view name, char const *payload,
                          std::size_t bytes, u32 capacity,
                          std::vector<Latency> const &runs,
                          std::string_view            context) {
    std::array<std::vector<double>, 4> values;
    for (std::size_t index = 0; index < runs.size(); ++index) {
        auto const &run = runs[index];
        std::println("raw metric=latency queue={} payload={} payload_bytes={} "
                     "capacity={} run={} p50_ns={:.3f} p90_ns={:.3f} "
                     "p99_ns={:.3f} p999_ns={:.3f} {}",
                     name, payload, bytes, capacity, index + 1, run.p50,
                     run.p90, run.p99, run.p999, context);
        values[0].push_back(run.p50);
        values[1].push_back(run.p90);
        values[2].push_back(run.p99);
        values[3].push_back(run.p999);
    }
    std::print("{:30}", name);
    constexpr std::array labels{"p50", "p90", "p99", "p999"};
    for (std::size_t index = 0; index < values.size(); ++index) {
        auto stats = stats_of(values[index]);
        std::print(" {}_mean={:.1f} ", labels[index], stats.mean);
        if (runs.size() > 1)
            std::print("95%CI=[{:.1f},{:.1f}]", stats.mean - stats.ci95,
                       stats.mean + stats.ci95);
        else
            std::print("95%CI=n/a");
    }
    std::println(" ns/rtt");
}

struct Cfg {
    u32              n    = kDefaultN;
    u32              runs = kDefaultRuns;
    std::vector<int> cpu_ps;
    int              cpu_c   = 8;
    bool             do_tp   = true;
    bool             do_pp   = true;
    bool             quick   = true;
    u32              cap     = 0;
    char const      *only    = nullptr;
    char const      *payload = nullptr;
    bool             list    = false;
};

template <class Suite>
void print_adapters(FILE *stream = stdout) {
    Suite::template adapters<u32, 1024>([stream](std::string_view name, auto) {
        std::println(stream, "{}", name);
    });
}

template <class Suite>
void validate_only(char const *only) {
    if (!only || !*only)
        return;
    std::string_view list = only;
    while (!list.empty()) {
        auto comma = list.find(',');
        auto name  = list.substr(0, comma);
        bool found = false;
        Suite::template adapters<u32, 1024>(
            [&](std::string_view candidate, auto) {
                found |= candidate == name;
            });
        if (name.empty() || !found) {
            std::println(stderr, "unknown adapter in --only: {}", name);
            print_adapters<Suite>(stderr);
            std::exit(2);
        }
        if (comma == std::string_view::npos)
            break;
        list.remove_prefix(comma + 1);
    }
}

inline void usage(char const *program) {
    std::println(stderr,
                 "Usage: {} [--throughput|--latency|--all] [--quick|--full] "
                 "[--scenario cross|smt] [--cpus P[,P...],C] [--only Q] "
                 "[--payload P] [--capacity N] [-n N] [-r RUNS]\n"
                 "  --cpus            producers first, consumer last; SPSC "
                 "needs two CPUs\n"
                 "  --scenario cross  SPSC: 6,8; MPSC: 6,8,10 (default)\n"
                 "  --scenario smt    SPSC: 6,7; MPSC: 6,8,7\n"
                 "  --only Q[,Q...]   canonical registry names\n"
                 "  --list            print canonical registry names\n"
                 "  --payload P       u32|u64|p16|p64\n"
                 "  --capacity N      64|1024|65536\n"
                 "  -n N              messages per producer\n"
                 "Env: QQU_N QQU_RUNS QQU_CPUS",
                 program);
}

template <class Suite>
Cfg parse(int argc, char **argv) {
    Cfg              cfg;
    std::string_view cpus     = Suite::default_cpus;
    bool             cpus_set = false;
    if (char const *env = std::getenv("QQU_N"))
        cfg.n = static_cast<u32>(std::strtoul(env, nullptr, 10));
    if (char const *env = std::getenv("QQU_RUNS"))
        cfg.runs = static_cast<u32>(std::strtoul(env, nullptr, 10));
    if (char const *env = std::getenv("QQU_CPUS")) {
        cpus     = env;
        cpus_set = true;
    }
    auto invalid = [&] {
        usage(argv[0]);
        std::exit(2);
    };
    for (int index = 1; index < argc; ++index) {
        std::string_view arg = argv[index];
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else if (arg == "--list") {
            cfg.list = true;
        } else if (arg == "--throughput") {
            cfg.do_tp = true;
            cfg.do_pp = false;
        } else if (arg == "--latency") {
            cfg.do_tp = false;
            cfg.do_pp = true;
        } else if (arg == "--all") {
            cfg.do_tp = cfg.do_pp = true;
        } else if (arg == "--quick") {
            cfg.quick = true;
        } else if (arg == "--full") {
            cfg.quick = false;
        } else if (arg == "--scenario" && index + 1 < argc) {
            std::string_view scenario = argv[++index];
            if (scenario != "cross" && scenario != "smt")
                invalid();
            if (!cpus_set)
                cpus =
                    scenario == "smt" ? Suite::smt_cpus : Suite::default_cpus;
        } else if (arg == "--cpus" && index + 1 < argc) {
            cpus     = argv[++index];
            cpus_set = true;
        } else if (arg == "--only" && index + 1 < argc) {
            cfg.only = argv[++index];
        } else if (arg == "--payload" && index + 1 < argc) {
            cfg.payload              = argv[++index];
            std::string_view payload = cfg.payload;
            if (payload != "u32" && payload != "u64" && payload != "p16" &&
                payload != "p64")
                invalid();
        } else if (arg == "--capacity" && index + 1 < argc) {
            cfg.cap =
                static_cast<u32>(std::strtoul(argv[++index], nullptr, 10));
            if (cfg.cap != 64 && cfg.cap != 1024 && cfg.cap != 65536)
                invalid();
        } else if (arg == "-n" && index + 1 < argc) {
            cfg.n = static_cast<u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (arg == "-r" && index + 1 < argc) {
            cfg.runs =
                static_cast<u32>(std::strtoul(argv[++index], nullptr, 10));
        } else {
            invalid();
        }
    }
    do {
        auto comma = cpus.find(',');
        auto item  = cpus.substr(0, comma);
        int  cpu   = -1;
        auto [end, error] =
            std::from_chars(item.data(), item.data() + item.size(), cpu);
        if (error != std::errc{} || end != item.data() + item.size() ||
            cpu < -1 || cpu >= CPU_SETSIZE)
            invalid();
        cfg.cpu_ps.push_back(cpu);
        if (comma == std::string_view::npos)
            break;
        cpus.remove_prefix(comma + 1);
    } while (true);
    if (cfg.cpu_ps.size() < 2 ||
        (!Suite::multi_producer && cfg.cpu_ps.size() != 2))
        invalid();
    cfg.cpu_c = cfg.cpu_ps.back();
    cfg.cpu_ps.pop_back();
    if (cfg.quick) {
        cfg.n    = std::min(cfg.n, 100'000u);
        cfg.runs = std::min(cfg.runs, 1u);
    }
    if (cfg.n == 0 || cfg.runs == 0) {
        std::println(stderr, "n and runs must be > 0");
        std::exit(2);
    }
    validate_only<Suite>(cfg.only);
    return cfg;
}

inline char const *pin_topology(int producer, int consumer) {
    auto location = [](int cpu) {
        std::array<int, 2> result{-1, -1};
        std::string        path =
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        std::ifstream(path + "physical_package_id") >> result[0];
        std::ifstream(path + "core_id") >> result[1];
        return result;
    };
    auto first = location(producer), second = location(consumer);
    if (first[0] < 0 || first[1] < 0 || second[0] < 0 || second[1] < 0)
        return "unknown";
    return first == second ? "same-core" : "cross-core";
}

template <class Q>
double once_throughput(Cfg const &cfg, Q &queue) {
    using T                     = typename Q::value_type;
    auto              producers = cfg.cpu_ps.size();
    clock::time_point start, finish;
    u64               sum = 0;
    std::barrier      ready(static_cast<std::ptrdiff_t>(producers + 1));
    std::barrier      timed(static_cast<std::ptrdiff_t>(producers + 1),
                            [&] { start = clock::now(); });
    auto              producer = [&](std::size_t index) {
        pin(cfg.cpu_ps[index]);
        ready.arrive_and_wait();
        for (u32 warm = 0; warm < kWarmThru; ++warm)
            queue.push(value<T>(0));
        timed.arrive_and_wait();
        for (u32 message = 0; message < cfg.n; ++message)
            queue.push(value<T>(message + 1));
    };
    auto                     consumer = std::thread([&] {
        pin(cfg.cpu_c);
        ready.arrive_and_wait();
        T message{};
        for (u64 warm = 0; warm < u64(kWarmThru) * producers; ++warm)
            queue.pop(message);
        timed.arrive_and_wait();
        for (u64 index = 0; index < u64(cfg.n) * producers; ++index) {
            queue.pop(message);
            sum += scalar(message);
        }
        finish = clock::now();
    });
    std::vector<std::thread> peers;
    for (std::size_t index = 1; index < producers; ++index)
        peers.emplace_back(producer, index);
    producer(0);
    for (auto &peer : peers)
        peer.join();
    consumer.join();
    u64 expected = u64(cfg.n) * (u64(cfg.n) + 1) / 2 * producers;
    if (sum != expected) {
        std::println(stderr, "checksum mismatch");
        std::exit(1);
    }
    return std::chrono::duration<double>(finish - start).count();
}

template <class Suite, class T, u32 Capacity>
void run_case(Cfg const &cfg, double nspt, char const *payload) {
    std::println("---- payload={} bytes={} capacity={} ----", payload,
                 sizeof(T), Capacity);
    auto context =
        std::format("suite={} producers={} latency_kind={} api=wait n={}",
                    Suite::name, cfg.cpu_ps.size(), Suite::latency_kind, cfg.n);
    if (cfg.do_tp) {
        std::vector<Competitor<double>> competitors;
        Suite::template adapters<T, Capacity>(
            [&](std::string_view name, auto type) {
                using Q = typename decltype(type)::type;
                competitors.push_back(
                    {name, {}, [&](auto &samples) {
                         auto queue = std::make_unique<Q>();
                         samples.push_back(double(cfg.n) * cfg.cpu_ps.size() /
                                           once_throughput(cfg, *queue));
                     }});
            });
        run_rounds(competitors, cfg.runs, cfg.only);
        for (auto &competitor : competitors)
            if (!competitor.samples.empty())
                print_throughput(competitor.name, payload, sizeof(T), Capacity,
                                 competitor.samples, context);
    }
    if (cfg.do_pp) {
        std::vector<Competitor<Latency>> competitors;
        Suite::template adapters<T, Capacity>(
            [&](std::string_view name, auto type) {
                using Q = typename decltype(type)::type;
                competitors.push_back(
                    {name, {}, [&](auto &runs) {
                         std::vector<double> samples(std::size_t(cfg.n) *
                                                     cfg.cpu_ps.size());
                         Suite::template latency<Q>(cfg, samples, nspt);
                         std::ranges::sort(samples);
                         runs.push_back({pct(samples, 50), pct(samples, 90),
                                         pct(samples, 99), pct(samples, 99.9)});
                     }});
            });
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
                print_latency(competitor.name, payload, sizeof(T), Capacity,
                              competitor.samples, context);
    }
}

template <class Suite, class T>
void run_payload(Cfg const &cfg, double nspt, char const *payload) {
    if (cfg.cap == 64)
        run_case<Suite, T, 64>(cfg, nspt, payload);
    else if (cfg.cap == 1024)
        run_case<Suite, T, 1024>(cfg, nspt, payload);
    else if (cfg.cap == 65536)
        run_case<Suite, T, 65536>(cfg, nspt, payload);
    else if (cfg.quick)
        run_case<Suite, T, 1024>(cfg, nspt, payload);
    else {
        run_case<Suite, T, 64>(cfg, nspt, payload);
        run_case<Suite, T, 1024>(cfg, nspt, payload);
        run_case<Suite, T, 65536>(cfg, nspt, payload);
    }
}

template <class Suite>
int benchmark_main(int argc, char **argv) {
    Cfg cfg = parse<Suite>(argc, argv);
    if (cfg.list) {
        print_adapters<Suite>();
        return 0;
    }
    double nspt = 0;
    if (cfg.do_pp) {
        pin(cfg.cpu_ps.front());
        nspt = ns_per_tsc_tick();
    }
    std::string cpus, topology;
    for (int cpu : cfg.cpu_ps) {
        cpus += std::to_string(cpu) + ',';
        if (!topology.empty())
            topology += ',';
        topology += pin_topology(cpu, cfg.cpu_c);
    }
    cpus += std::to_string(cfg.cpu_c);
    auto capacities = cfg.cap     ? std::to_string(cfg.cap)
                      : cfg.quick ? "1024"
                                  : "64,1024,65536";
    std::println(
        "# suite={} n={} runs={} cpus={} [{}] capacities={} payloads={} "
        "only={} ns/tsc_tick={:.4f} producers={} latency_kind={} api=wait",
        Suite::name, cfg.n, cfg.runs, cpus, topology, capacities,
        cfg.payload ? cfg.payload : "u32,u64,p16,p64",
        cfg.only ? cfg.only : "all", nspt, cfg.cpu_ps.size(),
        Suite::latency_kind);
    if (cfg.do_pp)
        print_tsc_overhead(nspt);
    if (!cfg.payload || std::string_view(cfg.payload) == "u32")
        run_payload<Suite, u32>(cfg, nspt, "u32");
    if (!cfg.payload || std::string_view(cfg.payload) == "u64")
        run_payload<Suite, u64>(cfg, nspt, "u64");
    if (!cfg.payload || std::string_view(cfg.payload) == "p16")
        run_payload<Suite, Payload16>(cfg, nspt, "p16");
    if (!cfg.payload || std::string_view(cfg.payload) == "p64")
        run_payload<Suite, Payload64>(cfg, nspt, "p64");
    return 0;
}

} // namespace qqu::benchmark