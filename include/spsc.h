#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <emmintrin.h>
#include <type_traits>
#include <utility>

namespace qqu {

static constexpr size_t DEFAULT_ALIGN = 64;

template <typename T, size_t N, size_t ALIGN = DEFAULT_ALIGN>
    requires(N > 1 && std::has_single_bit(N) &&
             std::is_trivially_copyable_v<T> &&
             std::is_trivially_destructible_v<T>)
class spsc {
    static constexpr size_t mask         = N - 1;
    static constexpr size_t shuffle_bits = N < (1u << 12) ? 0u : 6u;
    static constexpr size_t mask_elem    = (1u << shuffle_bits) - 1;
    static constexpr size_t mask_hi      = ~0u << (2 * shuffle_bits);

    enum : uint8_t {
        EMPTY  = 0,
        STORED = 1
    };

    alignas(ALIGN) std::atomic<size_t> _head{0};
    alignas(ALIGN) std::atomic<size_t> _tail{0};
    alignas(ALIGN) std::atomic<uint8_t> _states[N]{};
    alignas(ALIGN) T _elems[N]{};

    static auto remap(size_t i) noexcept -> size_t {
        if constexpr (shuffle_bits == 0)
            return i & mask;
        return ((i >> shuffle_bits) & mask_elem) |
               ((i & mask_elem) << shuffle_bits) | (i & (mask_hi & mask));
    }

    template <typename U>
    auto do_push(size_t head, U &&v) noexcept -> void {
        const size_t i = remap(head);
        while (_states[i].load(std::memory_order_acquire) != EMPTY)
            _mm_pause();
        _elems[i] = std::forward<U>(v);
        _states[i].store(STORED, std::memory_order_release);
    }

    auto do_pop(size_t tail) noexcept -> T {
        const size_t i = remap(tail);
        while (_states[i].load(std::memory_order_acquire) != STORED)
            _mm_pause();
        T v = _elems[i];
        _states[i].store(EMPTY, std::memory_order_release);
        return v;
    }

  public:
    spsc()                        = default;
    ~spsc()                       = default;
    spsc(const spsc &)            = delete;
    spsc &operator=(const spsc &) = delete;
    spsc(spsc &&)                 = delete;
    spsc &operator=(spsc &&)      = delete;

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
        const size_t head = _head.load(std::memory_order_relaxed);
        if (static_cast<int>(head - _tail.load(std::memory_order_relaxed)) >=
            static_cast<int>(N)) [[unlikely]]
            return false;
        _head.store(head + 1, std::memory_order_relaxed);
        do_push(head, T(std::forward<Args>(args)...));
        return true;
    }

    template <typename... Args>
    auto emplace(Args &&...args) noexcept -> void {
        const size_t head = _head.load(std::memory_order_relaxed);
        _head.store(head + 1, std::memory_order_relaxed);
        do_push(head, T(std::forward<Args>(args)...));
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
        const size_t tail = _tail.load(std::memory_order_relaxed);
        if (static_cast<int>(_head.load(std::memory_order_relaxed) - tail) <= 0)
            [[unlikely]]
            return false;
        _tail.store(tail + 1, std::memory_order_relaxed);
        v = do_pop(tail);
        return true;
    }

    auto pop(T &v) noexcept -> void {
        const size_t tail = _tail.load(std::memory_order_relaxed);
        _tail.store(tail + 1, std::memory_order_relaxed);
        v = do_pop(tail);
    }
};

} // namespace qqu
