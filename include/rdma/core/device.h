#pragma once

#include <infiniband/verbs.h>

#include <memory>
#include <string>
#include <vector>

#include "rdma/common/status.h"
#include "rdma/common/types.h"

namespace rdma {

// RDMA device wrapper with RAII
class RdmaDevice {
public:
    struct DeviceInfo {
        std::string name;
        uint64_t guid;
        uint32_t vendor_id;
        uint32_t vendor_part_id;
        int numa_node;
        int num_ports;
    };

    ~RdmaDevice();

    // Non-copyable
    RdmaDevice(const RdmaDevice&) = delete;
    RdmaDevice& operator=(const RdmaDevice&) = delete;

    // Moveable
    RdmaDevice(RdmaDevice&& other) noexcept;
    RdmaDevice& operator=(RdmaDevice&& other) noexcept;

    // Factory methods
    static Result<std::unique_ptr<RdmaDevice>> Open(const char* device_name = nullptr);
    static Result<std::unique_ptr<RdmaDevice>> OpenByNuma(int numa_node);

    // Get all available RDMA devices
    static std::vector<DeviceInfo> GetAvailableDevices();

    // Accessors
    ibv_context* GetContext() const { return context_; }
    const std::string& GetName() const { return name_; }
    int GetNumaNode() const { return numa_node_; }

    // Port info
    Result<ibv_port_attr> QueryPort(uint8_t port_num = 1) const;
    Result<ibv_gid> QueryGid(uint8_t port_num, int gid_index) const;

    // Device capabilities
    const ibv_device_attr& GetDeviceAttr() const { return device_attr_; }
    uint32_t GetMaxQp() const { return device_attr_.max_qp; }
    uint32_t GetMaxCq() const { return device_attr_.max_cq; }
    uint32_t GetMaxMr() const { return device_attr_.max_mr; }
    uint32_t GetMaxQpWr() const { return device_attr_.max_qp_wr; }
    uint32_t GetMaxSge() const { return device_attr_.max_sge; }
    uint32_t GetMaxCqe() const { return device_attr_.max_cqe; }

    // NUMA validation
    static bool ValidateNicNuma(const char* nic_name, int expected_numa);

private:
    RdmaDevice() = default;

    ibv_context* context_ = nullptr;
    ibv_device_attr device_attr_{};
    std::string name_;
    int numa_node_ = -1;

    static int GetDeviceNumaNode(const char* device_name);
};

}  // namespace rdma
