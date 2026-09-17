#pragma once

#include "common.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <emmintrin.h>
#include <type_traits>
#include <utility>

namespace qqu {

template <typename T, size_t N, size_t ALIGN = DEFAULT_ALIGN>
    requires(N > 1 && std::has_single_bit(N) &&
             std::is_trivially_copyable_v<T> &&
             std::is_trivially_destructible_v<T>)
class mpmc {
    static constexpr size_t mask = N - 1;

    struct cell {
        std::atomic<size_t> seq;
        T                   v;
    };

    alignas(ALIGN) std::atomic<size_t> _head{0};
    alignas(ALIGN) std::atomic<size_t> _tail{0};
    alignas(ALIGN) cell _cells[N];

  public:
    mpmc() {
        for (size_t i = 0; i < N; ++i)
            _cells[i].seq.store(i, std::memory_order_relaxed);
    }
    ~mpmc()                       = default;
    mpmc(const mpmc &)            = delete;
    mpmc &operator=(const mpmc &) = delete;
    mpmc(mpmc &&)                 = delete;
    mpmc &operator=(mpmc &&)      = delete;

    [[nodiscard]]
    static constexpr auto capacity() noexcept -> size_t {
        return N;
    }

    [[nodiscard]]
    auto size() const noexcept -> size_t {
        const int n = static_cast<int>(_head.load(std::memory_order_relaxed) -
                                       _tail.load(std::memory_order_relaxed));
        return n < 0 ? 0 : static_cast<size_t>(n);
    }

    [[nodiscard]]
    auto full() const noexcept -> bool {
        return size() >= N;
    }

    [[nodiscard]]
    auto empty() const noexcept -> bool {
        return size() == 0;
    }

    template <typename... Args>
    [[nodiscard]]
    auto try_emplace(Args &&...args) noexcept -> bool {
        T      v(std::forward<Args>(args)...);
        size_t head = _head.load(std::memory_order_acquire);
        cell  &c    = _cells[head & mask];
        if (c.seq.load(std::memory_order_acquire) != head) [[unlikely]]
            return false;
        if (!_head.compare_exchange_strong(head, head + 1,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed))
            return false;
        c.v = std::move(v);
        c.seq.store(head + 1, std::memory_order_release);
        return true;
    }

    template <typename... Args>
    auto emplace(Args &&...args) noexcept -> void {
        T            v(std::forward<Args>(args)...);
        const size_t head = _head.fetch_add(1, std::memory_order_relaxed);
        cell        &c    = _cells[head & mask];
        while (c.seq.load(std::memory_order_acquire) != head)
            _mm_pause();
        c.v = std::move(v);
        c.seq.store(head + 1, std::memory_order_release);
    }

    [[nodiscard]]
    auto try_push(const T &v) noexcept -> bool {
        return try_emplace(v);
    }

    [[nodiscard]]
    auto try_push(T &&v) noexcept -> bool {
        return try_emplace(std::move(v));
    }

    auto push(const T &v) noexcept -> void {
        emplace(v);
    }

    auto push(T &&v) noexcept -> void {
        emplace(std::move(v));
    }

    [[nodiscard]]
    auto try_pop(T &v) noexcept -> bool {
        size_t pos = _tail.load(std::memory_order_acquire);
        cell  &c   = _cells[pos & mask];
        if (c.seq.load(std::memory_order_acquire) != pos + 1) [[unlikely]]
            return false;
        if (!_tail.compare_exchange_strong(pos, pos + 1,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed))
            return false;
        v = c.v;
        c.seq.store(pos + N, std::memory_order_release);
        return true;
    }

    auto pop(T &v) noexcept -> void {
        const size_t pos = _tail.fetch_add(1, std::memory_order_relaxed);
        cell        &c   = _cells[pos & mask];
        while (c.seq.load(std::memory_order_acquire) != pos + 1)
            _mm_pause();
        v = c.v;
        c.seq.store(pos + N, std::memory_order_release);
    }
};

} // namespace qqu
