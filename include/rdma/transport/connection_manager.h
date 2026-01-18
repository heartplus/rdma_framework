#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "rdma/common/status.h"
#include "rdma/core/completion_queue.h"
#include "rdma/core/protection_domain.h"
#include "rdma/transport/connection.h"

namespace rdma {

// Connection Manager configuration
struct ConnectionManagerConfig {
    uint16_t max_connections = 1024;
    ConnectionConfig default_conn_config;
};

// Connection Manager for managing multiple connections
class ConnectionManager {
public:
    ~ConnectionManager();

    // Non-copyable
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    // Factory method
    static Result<std::unique_ptr<ConnectionManager>> Create(ProtectionDomain* pd,
            CompletionQueue* send_cq, CompletionQueue* recv_cq,
            const ConnectionManagerConfig& config = ConnectionManagerConfig());

    // Create a new connection
    Result<Connection*> CreateConnection(
            const ConnectionConfig& config = ConnectionConfig());

    // Get connection by ID
    Connection* GetConnection(uint16_t conn_id);

    // Remove connection
    void RemoveConnection(uint16_t conn_id);

    // Get all connections
    const std::vector<std::unique_ptr<Connection>>& GetAllConnections() const {
        return connections_;
    }

    // Get active connection count
    size_t GetActiveCount() const { return active_count_; }

    // Get maximum connections
    uint16_t GetMaxConnections() const { return config_.max_connections; }

private:
    ConnectionManager() = default;

    uint16_t AllocateConnId();
    void FreeConnId(uint16_t conn_id);

    ProtectionDomain* pd_ = nullptr;
    CompletionQueue* send_cq_ = nullptr;
    CompletionQueue* recv_cq_ = nullptr;
    ConnectionManagerConfig config_;

    std::vector<std::unique_ptr<Connection>> connections_;
    std::vector<uint16_t> free_conn_ids_;
    size_t active_count_ = 0;
};

}  // namespace rdma
