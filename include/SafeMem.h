// Simple safe memcpy wrapper to avoid UB when src/dst are null
#pragma once
#include <cstddef>
#include <cstring>

inline bool safe_memcpy(void* dest, const void* src, std::size_t n) {
    if (n == 0) return true;
    if (!dest || !src) return false;
    std::memcpy(dest, src, n);
    return true;
}
