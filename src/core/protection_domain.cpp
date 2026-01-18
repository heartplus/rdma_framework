#include "rdma/core/protection_domain.h"

#include "rdma/common/logging.h"

namespace rdma {

ProtectionDomain::~ProtectionDomain() {
    if (pd_) {
        ibv_dealloc_pd(pd_);
        pd_ = nullptr;
    }
}

ProtectionDomain::ProtectionDomain(ProtectionDomain&& other) noexcept
        : pd_(other.pd_), device_(other.device_) {
    other.pd_ = nullptr;
    other.device_ = nullptr;
}

ProtectionDomain& ProtectionDomain::operator=(ProtectionDomain&& other) noexcept {
    if (this != &other) {
        if (pd_) {
            ibv_dealloc_pd(pd_);
        }
        pd_ = other.pd_;
        device_ = other.device_;
        other.pd_ = nullptr;
        other.device_ = nullptr;
    }
    return *this;
}

Result<std::unique_ptr<ProtectionDomain>> ProtectionDomain::Create(RdmaDevice* device) {
    if (!device || !device->GetContext()) {
        return Status::InvalidArgument("Invalid device");
    }

    ibv_pd* pd = ibv_alloc_pd(device->GetContext());
    if (!pd) {
        return Status::IOError("Failed to allocate protection domain", errno);
    }

    auto domain = std::unique_ptr<ProtectionDomain>(new ProtectionDomain());
    domain->pd_ = pd;
    domain->device_ = device;

    RDMA_LOG_DEBUG("Created protection domain for device %s", device->GetName().c_str());

    return domain;
}

}  // namespace rdma
