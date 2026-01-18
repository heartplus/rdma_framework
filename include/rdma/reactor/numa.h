#pragma once

#include <string>

#include "rdma/common/status.h"
#include "rdma/common/types.h"

namespace rdma {

// NUMA utilities
class NumaUtils {
public:
    // Get NUMA node for a CPU core
    static int GetNumaNodeForCpu(int cpu_core);

    // Get NUMA node for a network device
    static int GetNumaNodeForDevice(const char* device_name);

    // Validate NIC is on expected NUMA node
    static bool ValidateNicNuma(const char* nic_name, int expected_numa);

    // Bind current thread to NUMA node
    static Status BindToNumaNode(int numa_node);

    // Bind current thread to CPU core
    static Status BindToCpu(int cpu_core);

    // Get current NUMA node
    static int GetCurrentNumaNode();

    // Get number of NUMA nodes
    static int GetNumaNodeCount();

    // Check if NUMA is available
    static bool IsNumaAvailable();
};

}  // namespace rdma
