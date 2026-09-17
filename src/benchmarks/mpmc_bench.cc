#include "common.hpp"
#include "mpmc_adapters.hpp"

#include <atomic>
#include <type_traits>

namespace {

using namespace qqu::benchmark;

struct alignas(64) Reply {
    std::atomic<u64> completed{0};
};

struct MpmcSuite {
    static constexpr std::string_view name           = "mpmc";
    static constexpr std::string_view default_cpus   = "2,4,6,8,10";
    static constexpr std::string_view smt_cpus       = "2,4,6,3,5";
    static constexpr std::string_view latency_kind   = "fan_in_ack_rtt";
    static constexpr bool             multi_producer = true;

    template <class T, u32 Capacity, class F>
    static void adapters(F &&visit, bool defaults_only = false) {
        for_each_mpmc_adapter<T, Capacity>(
            [&](std::string_view name, auto type_id) { visit(name, type_id); },
            defaults_only);
    }

    template <class Q>
    static void latency(Cfg const &cfg, std::span<double> samples,
                        double nspt) {
        using T                      = typename Q::value_type;
        auto               queue     = std::make_unique<Q>();
        auto               producers = cfg.cpu_ps.size();
        auto               consumers = cfg.cpu_cs.size();
        std::vector<Reply> replies(producers);
        std::vector<u64>   completed(producers);
        std::barrier       ready(static_cast<std::ptrdiff_t>(producers + consumers));
        std::barrier       timed(static_cast<std::ptrdiff_t>(producers + consumers));
        auto               producer = [&](std::size_t index) {
            pin(cfg.cpu_ps[index]);
            ready.arrive_and_wait();
            auto message = value<T>(static_cast<u32>(index));
            auto send    = [&](u64 sequence) {
                queue->push(message);
                while (replies[index].completed.load(
                           std::memory_order_acquire) != sequence)
                    _mm_pause();
            };
            for (u32 warm = 0; warm < kWarmLat; ++warm)
                send(u64(warm) + 1);
            timed.arrive_and_wait();
            auto output = samples.subspan(index * cfg.n, cfg.n);
            for (u32 sample = 0; sample < cfg.n; ++sample) {
                u64 start = tsc_start();
                send(u64(kWarmLat) + sample + 1);
                output[sample] = static_cast<double>(tsc_end() - start) * nspt;
            }
        };
        auto consumer = [&](std::size_t index) {
            pin(cfg.cpu_cs[index]);
            ready.arrive_and_wait();
            auto receive = [&](u64 count) {
                T message{};
                for (u64 i = 0; i < count; ++i) {
                    queue->pop(message);
                    auto sender = scalar(message);
                    replies[sender].completed.store(++completed[sender],
                                                    std::memory_order_release);
                }
            };
            receive(u64(kWarmLat) * producers / consumers);
            timed.arrive_and_wait();
            receive(u64(cfg.n) * producers / consumers);
        };
        std::vector<std::thread> producer_threads;
        std::vector<std::thread> consumer_threads;
        for (std::size_t index = 1; index < producers; ++index)
            producer_threads.emplace_back(producer, index);
        for (std::size_t index = 0; index < consumers; ++index)
            consumer_threads.emplace_back(consumer, index);
        producer(0);
        for (auto &peer : producer_threads)
            peer.join();
        for (auto &peer : consumer_threads)
            peer.join();
    }
};

} // namespace

int main(int argc, char **argv) {
    return benchmark_main<MpmcSuite>(argc, argv);
}
