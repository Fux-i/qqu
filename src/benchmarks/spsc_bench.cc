#include "adapters.hpp"
#include "common.hpp"

namespace {

using namespace qqu::benchmark;

struct SpscSuite {
    static constexpr std::string_view name           = "spsc";
    static constexpr std::string_view default_cpus   = "6,8";
    static constexpr std::string_view smt_cpus       = "6,7";
    static constexpr std::string_view latency_kind   = "ping_pong_rtt";
    static constexpr bool             multi_producer = false;

    template <class T, u32 Capacity, class F>
    static void adapters(F &&visit, bool defaults_only = false) {
        for_each_adapter<T, Capacity>(std::forward<F>(visit), defaults_only);
    }

    template <class Q>
    static void latency(Cfg const &cfg, std::span<double> samples,
                        double nspt) {
        using T               = typename Q::value_type;
        auto         request  = std::make_unique<Q>();
        auto         response = std::make_unique<Q>();
        std::barrier ready(2);
        auto         peer = std::thread([&] {
            pin(cfg.cpu_c);
            ready.arrive_and_wait();
            T message{};
            for (u64 index = 0; index < u64(kWarmLat) + cfg.n; ++index) {
                request->pop(message);
                response->push(message);
            }
        });

        pin(cfg.cpu_ps.front());
        ready.arrive_and_wait();
        T message{};
        for (u32 index = 0; index < kWarmLat; ++index) {
            request->push(value<T>(0));
            response->pop(message);
        }
        for (u32 index = 0; index < cfg.n; ++index) {
            u64 start = tsc_start();
            request->push(value<T>(index + 1));
            response->pop(message);
            samples[index] = static_cast<double>(tsc_end() - start) * nspt;
        }
        peer.join();
    }
};

} // namespace

int main(int argc, char **argv) {
    return benchmark_main<SpscSuite>(argc, argv);
}