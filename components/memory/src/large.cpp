#include "hc/memory/large.hpp"

#include <cstdlib>

namespace hc::memory {

namespace {

// written once by main before the tasks start, read-only afterwards
AllocateFunction allocate_function = std::malloc;
ReleaseFunction release_function = std::free;

} // namespace

void set_large_heap(AllocateFunction allocate, ReleaseFunction release)
{
    allocate_function = allocate;
    release_function = release;
}

void *allocate_large(size_t size)
{
    void *pointer = allocate_function(size == 0 ? 1 : size);
    if (pointer == nullptr) {
        std::abort();
    }
    return pointer;
}

void release_large(void *pointer)
{
    release_function(pointer);
}

} // namespace hc::memory
