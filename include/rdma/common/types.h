#pragma once

#include <cstddef>
#include <cstdint>

namespace rdma {

// Compiler hints for optimization
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#define HOT_FUNCTION __attribute__((hot))
#define COLD_FUNCTION __attribute__((cold))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// RPC operation codes
enum class RpcOpcode : uint8_t {
    REQUEST_SEND = 0,
    REQUEST_WRITE = 1,
    REQUEST_READ = 2,
    RESPONSE_SEND = 3,
    RECV = 4,
    CREDIT_UPDATE = 5,
    KEEPALIVE = 6,
};

// Connection states
enum class ConnectionState : uint8_t {
    DISCONNECTED = 0,
    CONNECTING = 1,
    CONNECTED = 2,
    ERROR = 3,
    CLOSING = 4,
};

// QP states
enum class QpState : uint8_t {
    RESET = 0,
    INIT = 1,
    RTR = 2,
    RTS = 3,
    ERROR = 4,
};

// Memory region access flags
struct MemoryAccess {
    bool local_write = true;
    bool remote_write = false;
    bool remote_read = false;
    bool relaxed_ordering = false;
};

// Remote memory information for RDMA operations
struct RemoteMemoryInfo {
    uint64_t addr;
    uint32_t rkey;
    uint32_t length;
};

// QP connection parameters for handshake
struct QpConnParams {
    uint16_t lid;           // Local ID (for IB)
    uint32_t qp_num;        // Queue Pair number
    uint32_t psn;           // Packet Sequence Number
    uint8_t gid[16];        // Global ID (for RoCE)
    uint16_t port_num;
    uint8_t gid_index;
    uint32_t mtu;
};

// Buffer allocation policy
enum class BufferAllocationPolicy {
    LIFO,   // Stack-style allocation (original)
    FIFO,   // Queue-style allocation (recommended for locality)
    SLAB    // Slab allocation by size (future extension)
};

// Reactor batch mode
enum class BatchMode {
    LATENCY,    // 8-16, low latency
    THROUGHPUT, // 32-64, high throughput
    ADAPTIVE,   // Dynamic adjustment
};

// NUMA configuration
struct NumaConfig {
    int cpu_core = -1;      // CPU core to bind (-1 = no binding)
    int numa_node = -1;     // NUMA node ID (-1 = any)
    const char* nic_name = nullptr;  // RDMA NIC device name
};

// Constants
constexpr size_t kCacheLineSize = 64;
constexpr size_t kPageSize = 4096;
constexpr size_t kHugePageSize = 2 * 1024 * 1024;  // 2MB huge page

}  // namespace rdma
