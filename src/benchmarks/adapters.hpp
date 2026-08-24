#pragma once

#include "spsc.h"

#include <atomic_queue/atomic_queue.h>
#include <rigtorp/SPSCQueue.h>

#include <cstddef>
#include <string_view>
#include <type_traits>

namespace qqu::benchmark {

template <class T, unsigned Capacity>
struct Qqu {
    using value_type = T;
    qqu::spsc<T, Capacity> q;

    void push(T const &value) noexcept {
        q.push(value);
    }
    void pop(T &value) noexcept {
        q.pop(value);
    }
};

template <class T, unsigned Capacity>
struct Rigtorp {
    using value_type = T;
    rigtorp::SPSCQueue<T> q{Capacity};

    void push(T const &value) noexcept {
        q.push(value);
    }
    void pop(T &value) noexcept {
        for (;;) {
            if (T *result = q.front()) {
                value = *result;
                q.pop();
                return;
            }
            __builtin_ia32_pause();
        }
    }
};

template <class T, unsigned Capacity>
struct AtomicQueue {
    using value_type = T;
    using Queue =
        atomic_queue::AtomicQueue2<T, Capacity, false, false, false, true>;
    Queue q;

    void push(T const &value) noexcept {
        q.push(value);
    }
    void pop(T &value) noexcept {
        value = q.pop();
    }
};

template <class Q, class T>
concept QueueAdapter = requires(Q queue, T const &input, T &output) {
    typename Q::value_type;
    { queue.push(input) } noexcept;
    { queue.pop(output) } noexcept;
};

template <class T, unsigned Capacity, class F>
void for_each_adapter(F &&f, bool defaults_only = false) {
    auto visit = [&]<class Q>(std::string_view name, bool is_default) {
        if (!defaults_only || is_default)
            f(name, std::type_identity<Q>{});
    };

    visit.template operator()<Qqu<T, Capacity>>("qqu::spsc", true);
    visit.template operator()<Rigtorp<T, Capacity>>("rigtorp::SPSCQueue", true);
    visit.template operator()<AtomicQueue<T, Capacity>>(
        "atomic_queue::AtomicQueue2", true);
}

} // namespace qqu::benchmark
