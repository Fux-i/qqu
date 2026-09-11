#pragma once

#include "common.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <emmintrin.h>
#include <type_traits>
#include <utility>

namespace qqu {

template <typename T, size_t N, size_t ALIGN = DEFAULT_ALIGN>
    requires(N > 1 && std::has_single_bit(N) &&
             std::is_trivially_copyable_v<T> &&
             std::is_trivially_destructible_v<T>)
class mpsc_aligned {
    static constexpr size_t mask       = N - 1;
    static constexpr size_t CACHE_LINE = 64;

    struct alignas(CACHE_LINE) slot {
        std::atomic<size_t> turn{0};
        T                   v;
    };

    alignas(ALIGN) std::atomic<size_t> _head{0};
    alignas(ALIGN) std::atomic<size_t> _tail{0};
    alignas(ALIGN) slot _slots[N];

    [[nodiscard]]
    static constexpr auto turn(size_t pos) noexcept -> size_t {
        return pos / N;
    }

    [[nodiscard]]
    static constexpr auto idx(size_t pos) noexcept -> size_t {
        return pos & mask;
    }

  public:
    mpsc_aligned()                                = default;
    ~mpsc_aligned()                               = default;
    mpsc_aligned(const mpsc_aligned &)            = delete;
    mpsc_aligned &operator=(const mpsc_aligned &) = delete;
    mpsc_aligned(mpsc_aligned &&)                 = delete;
    mpsc_aligned &operator=(mpsc_aligned &&)      = delete;

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
        T            v(std::forward<Args>(args)...);
        const size_t head_pos      = _head.load(std::memory_order_acquire);
        slot        &s             = _slots[idx(head_pos)];
        const size_t expected_turn = turn(head_pos) * 2;

        if (s.turn.load(std::memory_order_acquire) != expected_turn)
            return false;

        if (!_head.compare_exchange_strong(
                const_cast<size_t &>(head_pos), head_pos + 1,
                std::memory_order_relaxed, std::memory_order_relaxed))
            return false;

        s.v = std::move(v);
        s.turn.store(expected_turn + 1, std::memory_order_release);
        return true;
    }

    template <typename... Args>
    auto emplace(Args &&...args) noexcept -> void {
        T v(std::forward<Args>(args)...);
        while (!try_push(v))
            _mm_pause();
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
        while (!try_push(v))
            _mm_pause();
    }

    auto push(T &&v) noexcept -> void {
        while (!try_push(std::move(v)))
            _mm_pause();
    }

    [[nodiscard]]
    auto try_pop(T &v) noexcept -> bool {
        const size_t pos           = _tail.load(std::memory_order_relaxed);
        slot        &s             = _slots[idx(pos)];
        const size_t expected_turn = turn(pos) * 2 + 1;

        if (s.turn.load(std::memory_order_acquire) != expected_turn)
            [[unlikely]]
            return false;

        v = s.v;
        s.turn.store(expected_turn + 1, std::memory_order_release);
        _tail.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    auto pop(T &v) noexcept -> void {
        while (!try_pop(v))
            _mm_pause();
    }
};

} // namespace qqu
