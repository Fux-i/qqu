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
class mpsc_fetchadd {
    static constexpr size_t mask = N - 1;

    struct cell {
        std::atomic<size_t> seq;
        T                   v;
    };

    alignas(ALIGN) std::atomic<size_t> _head{0};
    alignas(ALIGN) std::atomic<size_t> _tail{0};
    alignas(ALIGN) cell _cells[N];

  public:
    mpsc_fetchadd() {
        for (size_t i = 0; i < N; ++i)
            _cells[i].seq.store(i, std::memory_order_relaxed);
    }
    ~mpsc_fetchadd()                                = default;
    mpsc_fetchadd(const mpsc_fetchadd &)            = delete;
    mpsc_fetchadd &operator=(const mpsc_fetchadd &) = delete;
    mpsc_fetchadd(mpsc_fetchadd &&)                 = delete;
    mpsc_fetchadd &operator=(mpsc_fetchadd &&)      = delete;

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
        T value(std::forward<Args>(args)...);
        size_t position = _head.load(std::memory_order_relaxed);
        for (;;) {
            auto &slot = _cells[position & mask];
            const auto difference = static_cast<intptr_t>(
                slot.seq.load(std::memory_order_acquire) - position);
            if (difference == 0) {
                if (_head.compare_exchange_weak(position, position + 1,
                                               std::memory_order_relaxed)) {
                    slot.v = std::move(value);
                    slot.seq.store(position + 1, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = _head.load(std::memory_order_relaxed);
            }
        }
    }

    template <typename... Args>
    auto emplace(Args &&...args) noexcept -> void {
        T value(std::forward<Args>(args)...);
        const size_t position = _head.fetch_add(1, std::memory_order_relaxed);
        auto &slot = _cells[position & mask];
        while (slot.seq.load(std::memory_order_acquire) != position)
            _mm_pause();
        slot.v = std::move(value);
        slot.seq.store(position + 1, std::memory_order_release);
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
        const size_t pos = _tail.load(std::memory_order_relaxed);
        cell        &c   = _cells[pos & mask];
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
