#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <utility>

namespace qqu {

static constexpr size_t DEFAULT_ALIGN = 64;

template <typename T, size_t N, size_t ALIGN = DEFAULT_ALIGN>
    requires(N > 1 && std::has_single_bit(N))
class spsc {
    static constexpr size_t mask = N - 1;

    alignas(ALIGN) std::array<T, N> _data{};
    alignas(ALIGN) std::atomic<size_t> _proi{0};
    alignas(ALIGN) std::atomic<size_t> _coni{0};

  public:
    static constexpr auto capacity() noexcept -> size_t {
        return N;
    }

    template <typename... Args>
    auto emplace(Args &&...args) -> bool {
        const size_t proi = _proi.load(std::memory_order_relaxed);
        if (proi - _coni.load(std::memory_order_acquire) == N)
            return false;
        _data[proi & mask] = T(std::forward<Args>(args)...);
        _proi.store(proi + 1, std::memory_order_release);
        return true;
    }

    auto push(T v) -> bool {
        return emplace(std::move(v));
    }

    auto pop(T &v) -> bool {
        const size_t coni = _coni.load(std::memory_order_relaxed);
        if (coni == _proi.load(std::memory_order_acquire))
            return false;
        v = std::move(_data[coni & mask]);
        _coni.store(coni + 1, std::memory_order_release);
        return true;
    }

    auto empty() const -> bool {
        return _proi.load(std::memory_order_acquire) == _coni.load(std::memory_order_acquire);
    }

    auto full() const -> bool {
        return _proi.load(std::memory_order_acquire) - _coni.load(std::memory_order_acquire) == N;
    }

    auto size() const -> size_t {
        return _proi.load(std::memory_order_acquire) - _coni.load(std::memory_order_acquire);
    }
};

} // namespace qqu
