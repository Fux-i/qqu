#pragma once

#include "common.h"

#include <algorithm>
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
class mpsc_remap {
    static constexpr size_t mask       = N - 1;
    static constexpr size_t CACHE_LINE = 64;

    struct cell {
        std::atomic<size_t> seq;
        T                   v;
    };

    static constexpr unsigned SHUFFLE_BITS = sizeof(cell) >= CACHE_LINE ? 0 :
        std::min(std::bit_width(CACHE_LINE / sizeof(cell)) - 1,
                 (std::bit_width(N) - 1) / 2);

    alignas(ALIGN) std::atomic<size_t> _head{0};
    alignas(ALIGN) std::atomic<size_t> _tail{0};
    alignas(ALIGN) cell _cells[N];

    [[nodiscard]]
    static auto remap_index(size_t idx) noexcept -> size_t {
        const size_t low_mask = (size_t{1} << SHUFFLE_BITS) - 1;
        const size_t high_mask = ~((size_t{1} << (2 * SHUFFLE_BITS)) - 1);
        return ((idx >> SHUFFLE_BITS) & low_mask) |
               ((idx & low_mask) << SHUFFLE_BITS) | (idx & high_mask);
    }

  public:
    mpsc_remap() {
        for (size_t i = 0; i < N; ++i)
            _cells[remap_index(i)].seq.store(i, std::memory_order_relaxed);
    }
    ~mpsc_remap()                             = default;
    mpsc_remap(const mpsc_remap &)            = delete;
    mpsc_remap &operator=(const mpsc_remap &) = delete;
    mpsc_remap(mpsc_remap &&)                 = delete;
    mpsc_remap &operator=(mpsc_remap &&)      = delete;

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
        size_t pos = _head.load(std::memory_order_relaxed);
        while (true) {
            const size_t idx = remap_index(pos & mask);
            cell        &c   = _cells[idx];

            const intptr_t diff =
                static_cast<intptr_t>(c.seq.load(std::memory_order_acquire)) -
                static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (_head.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
                    c.v = std::move(v);
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = _head.load(std::memory_order_relaxed);
            }
        }
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
        const size_t pos = _tail.load(std::memory_order_relaxed);
        const size_t idx = remap_index(pos & mask);
        cell        &c   = _cells[idx];
        if (c.seq.load(std::memory_order_acquire) != pos + 1) [[unlikely]]
            return false;
        v = c.v;
        c.seq.store(pos + N, std::memory_order_release);
        _tail.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    auto pop(T &v) noexcept -> void {
        while (!try_pop(v))
            _mm_pause();
    }
};

} // namespace qqu
