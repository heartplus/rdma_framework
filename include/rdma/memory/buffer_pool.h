#pragma once

#include <cstddef>
#include <cstdlib>
#include <memory>

#include "rdma/common/status.h"
#include "rdma/common/types.h"
#include "rdma/core/memory_region.h"
#include "rdma/core/protection_domain.h"
#include "rdma/memory/buffer.h"
#include "rdma/memory/hugepage_allocator.h"
#include "rdma/memory/memory_allocator.h"

namespace rdma {

// Local Buffer Pool configuration
struct BufferPoolConfig {
    size_t buffer_size = 4096;
    size_t buffer_count = 1024;
    size_t max_buffer_count = 65536;
    int numa_node = -1;
    bool use_hugepage = true;
    BufferAllocationPolicy policy = BufferAllocationPolicy::FIFO;
};

// Local Buffer Pool (per-Reactor, no locking needed)
// Design considerations:
// 1. Use Hugepages to reduce TLB misses
// 2. One-time pre-allocation of all buffers
// 3. NUMA-aware allocation
// 4. Fixed-size array instead of std::vector to avoid hot-path reallocation
// 5. FIFO allocation strategy for better physical locality
class LocalBufferPool {
public:
    ~LocalBufferPool();

    // Non-copyable
    LocalBufferPool(const LocalBufferPool&) = delete;
    LocalBufferPool& operator=(const LocalBufferPool&) = delete;

    // Factory method
    static Result<std::unique_ptr<LocalBufferPool>> Create(ProtectionDomain* pd,
            const BufferPoolConfig& config = BufferPoolConfig());

    // Allocate buffer (supports LIFO/FIFO strategy)
    ALWAYS_INLINE Buffer Allocate() {
        if (UNLIKELY(count_ == 0)) {
            return Buffer();  // No available buffer
        }

        size_t offset;
        if (policy_ == BufferAllocationPolicy::FIFO) {
            // FIFO: take from head (improves physical locality)
            offset = offset_ring_[head_];
            head_ = (head_ + 1) % buffer_count_;
        } else {
            // LIFO: take from tail (original strategy)
            tail_ = (tail_ - 1 + buffer_count_) % buffer_count_;
            offset = offset_ring_[tail_];
        }
        --count_;

        return Buffer(GetBufferPtr(offset), buffer_size_, lkey_);
    }

    // Free buffer (always put at tail)
    // Prefetch for better cache behavior on next allocation
    ALWAYS_INLINE void Free(const Buffer& buf) {
        size_t offset = static_cast<uint8_t*>(buf.GetAddr()) - static_cast<uint8_t*>(base_addr_);
        offset_ring_[tail_] = offset;
        tail_ = (tail_ + 1) % buffer_count_;
        ++count_;

        // Prefetch buffer header to L1 cache for next allocation
        __builtin_prefetch(buf.GetAddr(), 1, 3);
    }

    // Get available buffer count
    ALWAYS_INLINE size_t AvailableCount() const { return count_; }

    // Get allocation policy
    BufferAllocationPolicy GetPolicy() const { return policy_; }

    // Get buffer size
    size_t GetBufferSize() const { return buffer_size_; }

    // Get memory region info
    uint32_t GetLkey() const { return lkey_; }
    uint32_t GetRkey() const { return rkey_; }

    // Get remote memory info for a buffer
    RemoteMemoryInfo GetRemoteInfo(const Buffer& buf) const {
        return buf.ToRemoteInfo(rkey_);
    }

private:
    LocalBufferPool() = default;

    void* GetBufferPtr(size_t offset) const {
        return static_cast<uint8_t*>(base_addr_) + offset;
    }

    std::unique_ptr<MemoryRegion> mr_;
    std::unique_ptr<MemoryAllocator> allocator_;

    void* base_addr_ = nullptr;
    size_t total_size_ = 0;

    size_t* offset_ring_ = nullptr;  // Fixed-size ring array
    size_t head_ = 0;                // Head pointer (FIFO allocation position)
    size_t tail_ = 0;                // Tail pointer (free position)
    size_t count_ = 0;               // Current available count

    size_t buffer_size_ = 0;
    size_t buffer_count_ = 0;
    BufferAllocationPolicy policy_ = BufferAllocationPolicy::FIFO;

    uint32_t lkey_ = 0;
    uint32_t rkey_ = 0;
};

}  // namespace rdma
