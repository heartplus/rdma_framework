#include "rdma/memory/buffer_pool.h"

#include "rdma/common/logging.h"

namespace rdma {

LocalBufferPool::~LocalBufferPool() {
    if (offset_ring_) {
        free(offset_ring_);
        offset_ring_ = nullptr;
    }
    if (base_addr_ && allocator_) {
        allocator_->Free(base_addr_, total_size_);
        base_addr_ = nullptr;
    }
}

Result<std::unique_ptr<LocalBufferPool>> LocalBufferPool::Create(ProtectionDomain* pd,
        const BufferPoolConfig& config) {
    if (!pd) {
        return Status::InvalidArgument("Invalid protection domain");
    }
    if (config.buffer_count > config.max_buffer_count) {
        return Status::InvalidArgument("buffer_count exceeds max");
    }
    if (config.buffer_size == 0 || config.buffer_count == 0) {
        return Status::InvalidArgument("Invalid buffer configuration");
    }

    // Select allocator
    std::unique_ptr<MemoryAllocator> allocator;
    if (config.use_hugepage) {
        allocator = std::make_unique<HugepageAllocator>(config.numa_node);
    } else {
        allocator = std::make_unique<DefaultAllocator>();
    }

    // Allocate entire memory block
    size_t total_size = config.buffer_size * config.buffer_count;
    void* memory = allocator->Allocate(total_size, kPageSize);
    if (!memory) {
        return Status::ResourceExhausted("Failed to allocate buffer pool memory");
    }

    // Register as single large MR
    auto mr_result = MemoryRegion::Register(pd, memory, total_size);
    if (!mr_result.ok()) {
        allocator->Free(memory, total_size);
        return mr_result.status();
    }

    // Create pool
    auto pool = std::unique_ptr<LocalBufferPool>(new LocalBufferPool());
    pool->mr_ = std::move(mr_result.value());
    pool->allocator_ = std::move(allocator);
    pool->base_addr_ = memory;
    pool->total_size_ = total_size;
    pool->buffer_size_ = config.buffer_size;
    pool->buffer_count_ = config.buffer_count;
    pool->policy_ = config.policy;
    pool->lkey_ = pool->mr_->GetLkey();
    pool->rkey_ = pool->mr_->GetRkey();

    // Allocate fixed-size offset ring
    pool->offset_ring_ = static_cast<size_t*>(aligned_alloc(64, config.buffer_count * sizeof(size_t)));
    if (!pool->offset_ring_) {
        return Status::ResourceExhausted("Failed to allocate offset ring");
    }

    // Pre-fill (sequential fill for FIFO)
    for (size_t i = 0; i < config.buffer_count; ++i) {
        pool->offset_ring_[i] = i * config.buffer_size;
    }

    // Initialize queue pointers
    pool->head_ = 0;
    pool->tail_ = config.buffer_count;
    pool->count_ = config.buffer_count;

    RDMA_LOG_INFO("Created buffer pool: buffer_size=%zu, buffer_count=%zu, total_size=%zu",
            config.buffer_size, config.buffer_count, total_size);

    return pool;
}

}  // namespace rdma
