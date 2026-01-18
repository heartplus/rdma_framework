#pragma once

#include <infiniband/verbs.h>

#include <memory>

#include "rdma/common/status.h"
#include "rdma/common/types.h"
#include "rdma/core/protection_domain.h"

namespace rdma {

// Forward declaration
class Buffer;

// Memory Region configuration
struct MemoryRegionConfig {
    bool enable_local_write = true;
    bool enable_remote_write = true;
    bool enable_remote_read = true;
    bool enable_relaxed_ordering = false;

    int GetAccessFlags() const {
        int flags = 0;
        if (enable_local_write) flags |= IBV_ACCESS_LOCAL_WRITE;
        if (enable_remote_write) flags |= IBV_ACCESS_REMOTE_WRITE;
        if (enable_remote_read) flags |= IBV_ACCESS_REMOTE_READ;
#ifdef IBV_ACCESS_RELAXED_ORDERING
        if (enable_relaxed_ordering) flags |= IBV_ACCESS_RELAXED_ORDERING;
#endif
        return flags;
    }
};

// Memory Region wrapper with RAII
class MemoryRegion {
public:
    ~MemoryRegion();

    // Non-copyable
    MemoryRegion(const MemoryRegion&) = delete;
    MemoryRegion& operator=(const MemoryRegion&) = delete;

    // Moveable
    MemoryRegion(MemoryRegion&& other) noexcept;
    MemoryRegion& operator=(MemoryRegion&& other) noexcept;

    // Factory methods
    static Result<std::unique_ptr<MemoryRegion>> Register(
            ProtectionDomain* pd, void* addr, size_t size,
            const MemoryRegionConfig& config = MemoryRegionConfig());

    // Accessors
    ibv_mr* GetRawMR() const { return mr_; }
    void* GetAddr() const { return addr_; }
    size_t GetSize() const { return size_; }
    uint32_t GetLkey() const { return mr_ ? mr_->lkey : 0; }
    uint32_t GetRkey() const { return mr_ ? mr_->rkey : 0; }

    // Get a buffer view into this MR
    Buffer GetBuffer(size_t offset, size_t length) const;

    // Get raw pointer at offset
    void* GetBufferPtr(size_t offset) const {
        return static_cast<uint8_t*>(addr_) + offset;
    }

    // Get remote memory info for RDMA operations
    RemoteMemoryInfo GetRemoteInfo() const {
        return RemoteMemoryInfo{reinterpret_cast<uint64_t>(addr_), GetRkey(),
                static_cast<uint32_t>(size_)};
    }

    RemoteMemoryInfo GetRemoteInfo(size_t offset, size_t length) const {
        return RemoteMemoryInfo{reinterpret_cast<uint64_t>(static_cast<uint8_t*>(addr_) + offset),
                GetRkey(), static_cast<uint32_t>(length)};
    }

private:
    MemoryRegion() = default;

    ibv_mr* mr_ = nullptr;
    void* addr_ = nullptr;
    size_t size_ = 0;
};

}  // namespace rdma
