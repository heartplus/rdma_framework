#pragma once

#include <cstddef>

#include "rdma/memory/memory_allocator.h"

namespace rdma {

// Hugepage allocator for NUMA-aware, TLB-efficient memory
// Design considerations:
// 1. Use Hugepages to reduce TLB misses
// 2. NUMA-aware allocation using mbind
// 3. Fallback to regular pages if hugepages unavailable
class HugepageAllocator : public MemoryAllocator {
public:
    explicit HugepageAllocator(int numa_node = -1);

    void* Allocate(size_t size, size_t alignment) override;
    void Free(void* ptr, size_t size) override;

    int GetNumaNode() const { return numa_node_; }

private:
    int numa_node_;
    bool use_1gb_pages_;
};

}  // namespace rdma
