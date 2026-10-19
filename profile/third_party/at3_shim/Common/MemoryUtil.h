#pragma once
// Stand-in for PPSSPP's aligned allocation helpers.
#include <cstddef>
#include <cstdlib>

inline void *AllocateAlignedMemory(std::size_t size, std::size_t alignment) {
    if (size == 0) size = 1;
#if defined(_WIN32)
    // Windows has no posix_memalign; _aligned_malloc must be paired with
    // _aligned_free (handled by the matching free below).
    return _aligned_malloc(size, alignment);
#else
    void *ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) return nullptr;
    return ptr;
#endif
}
inline void FreeAlignedMemory(void *ptr) {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}
