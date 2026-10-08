#pragma once

// The heap for large, long-lived data: the dashboard model and the trees of an import
// (docs/architecture.md section 11: the dashboard lives in PSRAM). Plain C++, host tested.
//
// On the device main points it at PSRAM before any task starts; on the PC it is malloc. The
// containers below allocate from it, so the code that uses them does not know where it lives.
// Small values that are read often stay out of it, in internal RAM.

#include <cstddef>
#include <new>
#include <string>
#include <vector>

namespace hc::memory {

using AllocateFunction = void *(*)(size_t size);
using ReleaseFunction = void (*)(void *pointer);

// Called once at start, before anything allocates from the large heap.
void set_large_heap(AllocateFunction allocate, ReleaseFunction release);

// Aborts when the heap is full: there is no way to go on without the dashboard model.
void *allocate_large(size_t size);
void release_large(void *pointer);

template <class T>
struct LargeAllocator {
    using value_type = T;

    LargeAllocator() = default;
    template <class U>
    LargeAllocator(const LargeAllocator<U> &) noexcept
    {
    }

    T *allocate(size_t count)
    {
        return static_cast<T *>(allocate_large(count * sizeof(T)));
    }
    void deallocate(T *pointer, size_t) noexcept
    {
        release_large(pointer);
    }

    template <class U>
    bool operator==(const LargeAllocator<U> &) const noexcept
    {
        return true;
    }
};

template <class T>
using Vector = std::vector<T, LargeAllocator<T>>;
using String = std::basic_string<char, std::char_traits<char>, LargeAllocator<char>>;

} // namespace hc::memory
