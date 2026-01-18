#include "rdma/core/memory_region.h"

#include "rdma/common/logging.h"
#include "rdma/memory/buffer.h"

namespace rdma {

MemoryRegion::~MemoryRegion() {
    if (mr_) {
        ibv_dereg_mr(mr_);
        mr_ = nullptr;
    }
}

MemoryRegion::MemoryRegion(MemoryRegion&& other) noexcept
        : mr_(other.mr_), addr_(other.addr_), size_(other.size_) {
    other.mr_ = nullptr;
    other.addr_ = nullptr;
    other.size_ = 0;
}

MemoryRegion& MemoryRegion::operator=(MemoryRegion&& other) noexcept {
    if (this != &other) {
        if (mr_) {
            ibv_dereg_mr(mr_);
        }
        mr_ = other.mr_;
        addr_ = other.addr_;
        size_ = other.size_;
        other.mr_ = nullptr;
        other.addr_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

Result<std::unique_ptr<MemoryRegion>> MemoryRegion::Register(ProtectionDomain* pd, void* addr,
        size_t size, const MemoryRegionConfig& config) {
    if (!pd || !pd->GetRawPD()) {
        return Status::InvalidArgument("Invalid protection domain");
    }
    if (!addr || size == 0) {
        return Status::InvalidArgument("Invalid memory region parameters");
    }

    ibv_mr* mr = ibv_reg_mr(pd->GetRawPD(), addr, size, config.GetAccessFlags());
    if (!mr) {
        return Status::IOError("Failed to register memory region", errno);
    }

    auto region = std::unique_ptr<MemoryRegion>(new MemoryRegion());
    region->mr_ = mr;
    region->addr_ = addr;
    region->size_ = size;

    RDMA_LOG_DEBUG("Registered memory region: addr=%p, size=%zu, lkey=0x%x, rkey=0x%x", addr, size,
            mr->lkey, mr->rkey);

    return region;
}

Buffer MemoryRegion::GetBuffer(size_t offset, size_t length) const {
    if (offset + length > size_) {
        return Buffer();
    }
    return Buffer(static_cast<uint8_t*>(addr_) + offset, length, GetLkey());
}

}  // namespace rdma
