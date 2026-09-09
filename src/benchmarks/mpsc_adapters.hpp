#pragma once

#include "mpsc.h"
#include "mpsc_aligned.h"
#include "mpsc_fetchadd.h"
#include "mpsc_remap.h"

#include <atomic_queue/atomic_queue.h>
#include <rigtorp/MPMCQueue.h>

#include <cstddef>
#include <string_view>
#include <type_traits>

namespace qqu::benchmark {

template <class T, unsigned Capacity>
struct QquMpsc {
    using value_type = T;
    qqu::mpsc<T, Capacity> q;

    void push(T const &value) noexcept {
        q.push(value);
    }
    void pop(T &value) noexcept {
        q.pop(value);
    }
};

template <class T, unsigned Capacity>
struct AtomicMpsc {
    using value_type = T;
    atomic_queue::AtomicQueue2<T, Capacity> q;

    void push(T const &value) noexcept {
        q.push(value);
    }
    void pop(T &value) noexcept {
        value = q.pop();
    }
};

template <class T, unsigned Capacity>
struct RigtorpMpsc {
    using value_type = T;
    rigtorp::MPMCQueue<T> q{Capacity};

    void push(T const &value) noexcept {
        q.push(value);
    }
    void pop(T &value) noexcept {
        q.pop(value);
    }
};

template <class T, unsigned Capacity, class F>
void for_each_mpsc_adapter(F &&f, bool defaults_only = false) {
    auto visit = [&]<class Q>(std::string_view name, bool is_default) {
        if (!defaults_only || is_default)
            f(name, std::type_identity<Q>{});
    };

    visit.template operator()<QquMpsc<T, Capacity>>("qqu::mpsc", true);
    visit.template operator()<AtomicMpsc<T, Capacity>>(
        "atomic_queue::AtomicQueue2", true);
    visit.template operator()<RigtorpMpsc<T, Capacity>>("rigtorp::MPMCQueue",
                                                        true);
}

} // namespace qqu::benchmark
