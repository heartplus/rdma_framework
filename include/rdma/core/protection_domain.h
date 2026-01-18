#pragma once

#include <infiniband/verbs.h>

#include <memory>

#include "rdma/common/status.h"
#include "rdma/core/device.h"

namespace rdma {

// Protection Domain wrapper with RAII
class ProtectionDomain {
public:
    ~ProtectionDomain();

    // Non-copyable
    ProtectionDomain(const ProtectionDomain&) = delete;
    ProtectionDomain& operator=(const ProtectionDomain&) = delete;

    // Moveable
    ProtectionDomain(ProtectionDomain&& other) noexcept;
    ProtectionDomain& operator=(ProtectionDomain&& other) noexcept;

    // Factory method
    static Result<std::unique_ptr<ProtectionDomain>> Create(RdmaDevice* device);

    // Accessors
    ibv_pd* GetRawPD() const { return pd_; }
    RdmaDevice* GetDevice() const { return device_; }

private:
    ProtectionDomain() = default;

    ibv_pd* pd_ = nullptr;
    RdmaDevice* device_ = nullptr;
};

}  // namespace rdma
