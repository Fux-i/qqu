#pragma once

#include "common.h"
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <utility>

namespace qqu {

static constexpr size_t DEFAULT_ALIGN = 64;

template <typename T, size_t N, size_t ALIGN = DEFAULT_ALIGN>
    requires(N > 1 && std::has_single_bit(N) &&
             std::is_nothrow_default_constructible_v<T>)
class spsc {
    static constexpr size_t mask = N - 1;

    alignas(ALIGN) std::array<T, N> _data{};
    alignas(ALIGN) std::atomic<size_t> _proi{0};
    size_t _cached_coni{};
    alignas(ALIGN) std::atomic<size_t> _coni{0};
    size_t _cached_proi{};

  public:
    [[nodiscard]]
    static constexpr auto capacity() noexcept -> size_t {
        return N;
    }

    [[nodiscard]]
    auto size() const noexcept -> size_t {
        return _proi.load(std::memory_order_acquire) -
               _coni.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    auto full() const noexcept -> bool {
        return size() == N;
    }

    [[nodiscard]]
    auto empty() const noexcept -> bool {
        return size() == 0;
    }

    template <typename... Args>
    [[nodiscard]]
    auto try_emplace(Args &&...args) noexcept -> bool {
        const size_t proi = _proi.load(std::memory_order_relaxed);
        if (proi - _cached_coni == N) {
            _cached_coni = _coni.load(std::memory_order_acquire);
            if (proi - _cached_coni == N)
                return false;
        }
        _data[proi & mask] = T(std::forward<Args>(args)...);
        _proi.store(proi + 1, std::memory_order_release);
        return true;
    }

    template <typename... Args>
    auto emplace(Args &&...args) noexcept -> void {
        while (!try_emplace(std::forward<Args>(args)...))
            pause_spin();
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
        while (!try_emplace(v))
            pause_spin();
    }

    auto push(T &&v) noexcept -> void {
        while (!try_emplace(std::move(v)))
            pause_spin();
    }

    [[nodiscard]]
    auto try_pop(T &v) noexcept -> bool {
        const size_t coni = _coni.load(std::memory_order_relaxed);
        if (coni == _cached_proi) {
            _cached_proi = _proi.load(std::memory_order_acquire);
            if (coni == _cached_proi)
                return false;
        }
        v = std::move(_data[coni & mask]);
        _coni.store(coni + 1, std::memory_order_release);
        return true;
    }

    auto pop(T &v) noexcept -> void {
        while (!try_pop(v))
            pause_spin();
    }
};

} // namespace qqu
