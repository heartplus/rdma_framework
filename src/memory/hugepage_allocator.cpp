#include "rdma/memory/hugepage_allocator.h"

#include <sys/mman.h>

#include <cstdlib>

#include "rdma/common/logging.h"

#ifdef RDMA_HAS_NUMA
#include <numa.h>
#include <numaif.h>
#endif

namespace rdma {

// DefaultAllocator implementation
void* DefaultAllocator::Allocate(size_t size, size_t alignment) {
    void* ptr = aligned_alloc(alignment, size);
    return ptr;
}

void DefaultAllocator::Free(void* ptr, size_t /*size*/) {
    free(ptr);
}

// HugepageAllocator implementation
HugepageAllocator::HugepageAllocator(int numa_node)
        : numa_node_(numa_node), use_1gb_pages_(false) {}

void* HugepageAllocator::Allocate(size_t size, size_t alignment) {
    (void)alignment;  // Hugepages are always page-aligned

    // Try 2MB huge pages
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB;

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);

    if (ptr == MAP_FAILED) {
        // Fallback to regular pages
        RDMA_LOG_WARN("Hugepage allocation failed, falling back to regular pages");
        ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            return nullptr;
        }
    }

#ifdef RDMA_HAS_NUMA
    // Bind to NUMA node if specified
    if (numa_node_ >= 0 && numa_available() >= 0) {
        unsigned long nodemask = 1UL << numa_node_;
        if (mbind(ptr, size, MPOL_BIND, &nodemask, sizeof(nodemask) * 8, 0) != 0) {
            RDMA_LOG_WARN("Failed to bind memory to NUMA node %d", numa_node_);
        }
    }
#endif

    return ptr;
}

void HugepageAllocator::Free(void* ptr, size_t size) {
    if (ptr) {
        munmap(ptr, size);
    }
}

}  // namespace rdma
