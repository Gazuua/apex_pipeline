#pragma once

#include <cstddef>

namespace apex::core::detail {

constexpr size_t next_power_of_2(size_t v) {
    if (v == 0) return 1;
    // Already a power of 2 — return as-is (prevents overflow)
    if ((v & (v - 1)) == 0) return v;
    // Overflow guard: cannot represent next power of 2
    if (v > (SIZE_MAX >> 1) + 1) return 0;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

} // namespace apex::core::detail
