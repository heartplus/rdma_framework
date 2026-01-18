#pragma once

#include <cstddef>

namespace rdma {

// Memory allocator interface
class MemoryAllocator {
public:
    virtual ~MemoryAllocator() = default;

    // Allocate aligned memory
    virtual void* Allocate(size_t size, size_t alignment) = 0;

    // Free memory
    virtual void Free(void* ptr, size_t size) = 0;
};

// Default allocator using aligned_alloc
class DefaultAllocator : public MemoryAllocator {
public:
    void* Allocate(size_t size, size_t alignment) override;
    void Free(void* ptr, size_t size) override;
};

}  // namespace rdma
