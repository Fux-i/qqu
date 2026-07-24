#pragma once

#if !defined(__linux__) || !defined(__x86_64__)
#error "qqu targets Linux only"
#endif

inline void pause_spin() noexcept {
    __builtin_ia32_pause();
}
