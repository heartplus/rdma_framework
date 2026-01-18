#pragma once

// RDMA Framework - Unified header file

// Common utilities
#include "rdma/common/logging.h"
#include "rdma/common/mpsc_queue.h"
#include "rdma/common/status.h"
#include "rdma/common/timing_wheel.h"
#include "rdma/common/types.h"
#include "rdma/common/wr_id.h"

// Core RDMA resources
#include "rdma/core/completion_queue.h"
#include "rdma/core/device.h"
#include "rdma/core/doorbell_batcher.h"
#include "rdma/core/memory_region.h"
#include "rdma/core/protection_domain.h"
#include "rdma/core/queue_pair.h"

// Memory management
#include "rdma/memory/buffer.h"
#include "rdma/memory/buffer_pool.h"
#include "rdma/memory/hugepage_allocator.h"
#include "rdma/memory/memory_allocator.h"

// Transport layer
#include "rdma/transport/connection.h"
#include "rdma/transport/connection_manager.h"
#include "rdma/transport/flow_control.h"
#include "rdma/transport/handshake_channel.h"
#include "rdma/transport/tcp_handshake.h"

// RPC layer
#include "rdma/rpc/completion_dispatcher.h"
#include "rdma/rpc/rpc_endpoint.h"
#include "rdma/rpc/rpc_slot.h"

// Reactor
#include "rdma/reactor/numa.h"
#include "rdma/reactor/reactor.h"

namespace rdma {

// Version information
constexpr int VERSION_MAJOR = 1;
constexpr int VERSION_MINOR = 0;
constexpr int VERSION_PATCH = 0;

inline const char* GetVersionString() {
    return "1.0.0";
}

}  // namespace rdma
