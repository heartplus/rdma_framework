#include "rdma/core/device.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include "rdma/common/logging.h"

namespace rdma {

RdmaDevice::~RdmaDevice() {
    if (context_) {
        ibv_close_device(context_);
        context_ = nullptr;
    }
}

RdmaDevice::RdmaDevice(RdmaDevice&& other) noexcept
        : context_(other.context_),
          device_attr_(other.device_attr_),
          name_(std::move(other.name_)),
          numa_node_(other.numa_node_) {
    other.context_ = nullptr;
}

RdmaDevice& RdmaDevice::operator=(RdmaDevice&& other) noexcept {
    if (this != &other) {
        if (context_) {
            ibv_close_device(context_);
        }
        context_ = other.context_;
        device_attr_ = other.device_attr_;
        name_ = std::move(other.name_);
        numa_node_ = other.numa_node_;
        other.context_ = nullptr;
    }
    return *this;
}

Result<std::unique_ptr<RdmaDevice>> RdmaDevice::Open(const char* device_name) {
    int num_devices;
    ibv_device** device_list = ibv_get_device_list(&num_devices);
    if (!device_list || num_devices == 0) {
        if (device_list) ibv_free_device_list(device_list);
        return Status::NotFound("No RDMA devices found");
    }

    ibv_device* target_device = nullptr;

    if (device_name) {
        // Find specific device
        for (int i = 0; i < num_devices; ++i) {
            if (strcmp(ibv_get_device_name(device_list[i]), device_name) == 0) {
                target_device = device_list[i];
                break;
            }
        }
        if (!target_device) {
            ibv_free_device_list(device_list);
            return Status::NotFound("RDMA device not found");
        }
    } else {
        // Use first device
        target_device = device_list[0];
    }

    ibv_context* context = ibv_open_device(target_device);
    if (!context) {
        ibv_free_device_list(device_list);
        return Status::IOError("Failed to open RDMA device", errno);
    }

    auto device = std::unique_ptr<RdmaDevice>(new RdmaDevice());
    device->context_ = context;
    device->name_ = ibv_get_device_name(target_device);
    device->numa_node_ = GetDeviceNumaNode(device->name_.c_str());

    // Query device attributes
    if (ibv_query_device(context, &device->device_attr_) != 0) {
        ibv_free_device_list(device_list);
        return Status::IOError("Failed to query device attributes", errno);
    }

    ibv_free_device_list(device_list);

    RDMA_LOG_INFO("Opened RDMA device: %s (NUMA node: %d)", device->name_.c_str(),
            device->numa_node_);

    return device;
}

Result<std::unique_ptr<RdmaDevice>> RdmaDevice::OpenByNuma(int numa_node) {
    int num_devices;
    ibv_device** device_list = ibv_get_device_list(&num_devices);
    if (!device_list || num_devices == 0) {
        if (device_list) ibv_free_device_list(device_list);
        return Status::NotFound("No RDMA devices found");
    }

    ibv_device* target_device = nullptr;

    for (int i = 0; i < num_devices; ++i) {
        const char* name = ibv_get_device_name(device_list[i]);
        int device_numa = GetDeviceNumaNode(name);
        if (device_numa == numa_node) {
            target_device = device_list[i];
            break;
        }
    }

    if (!target_device) {
        ibv_free_device_list(device_list);
        return Status::NotFound("No RDMA device found on specified NUMA node");
    }

    ibv_context* context = ibv_open_device(target_device);
    if (!context) {
        ibv_free_device_list(device_list);
        return Status::IOError("Failed to open RDMA device", errno);
    }

    auto device = std::unique_ptr<RdmaDevice>(new RdmaDevice());
    device->context_ = context;
    device->name_ = ibv_get_device_name(target_device);
    device->numa_node_ = numa_node;

    if (ibv_query_device(context, &device->device_attr_) != 0) {
        ibv_free_device_list(device_list);
        return Status::IOError("Failed to query device attributes", errno);
    }

    ibv_free_device_list(device_list);

    return device;
}

std::vector<RdmaDevice::DeviceInfo> RdmaDevice::GetAvailableDevices() {
    std::vector<DeviceInfo> devices;

    int num_devices;
    ibv_device** device_list = ibv_get_device_list(&num_devices);
    if (!device_list) {
        return devices;
    }

    for (int i = 0; i < num_devices; ++i) {
        DeviceInfo info;
        info.name = ibv_get_device_name(device_list[i]);
        info.guid = ibv_get_device_guid(device_list[i]);
        info.numa_node = GetDeviceNumaNode(info.name.c_str());

        // Open device temporarily to get more info
        ibv_context* ctx = ibv_open_device(device_list[i]);
        if (ctx) {
            ibv_device_attr attr;
            if (ibv_query_device(ctx, &attr) == 0) {
                info.vendor_id = attr.vendor_id;
                info.vendor_part_id = attr.vendor_part_id;
                info.num_ports = attr.phys_port_cnt;
            }
            ibv_close_device(ctx);
        }

        devices.push_back(info);
    }

    ibv_free_device_list(device_list);
    return devices;
}

Result<ibv_port_attr> RdmaDevice::QueryPort(uint8_t port_num) const {
    ibv_port_attr port_attr;
    if (ibv_query_port(context_, port_num, &port_attr) != 0) {
        return Status::IOError("Failed to query port", errno);
    }
    return port_attr;
}

Result<ibv_gid> RdmaDevice::QueryGid(uint8_t port_num, int gid_index) const {
    ibv_gid gid;
    if (ibv_query_gid(context_, port_num, gid_index, &gid) != 0) {
        return Status::IOError("Failed to query GID", errno);
    }
    return gid;
}

bool RdmaDevice::ValidateNicNuma(const char* nic_name, int expected_numa) {
    int actual_numa = GetDeviceNumaNode(nic_name);
    return actual_numa == expected_numa;
}

int RdmaDevice::GetDeviceNumaNode(const char* device_name) {
    // Try to read from sysfs
    std::string path = "/sys/class/infiniband/";
    path += device_name;
    path += "/device/numa_node";

    std::ifstream file(path);
    if (file.is_open()) {
        int numa_node;
        file >> numa_node;
        return numa_node;
    }

    return -1;  // Unknown
}

}  // namespace rdma
