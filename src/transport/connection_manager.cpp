#include "rdma/transport/connection_manager.h"

#include "rdma/common/logging.h"

namespace rdma {

ConnectionManager::~ConnectionManager() {
    connections_.clear();
}

Result<std::unique_ptr<ConnectionManager>> ConnectionManager::Create(ProtectionDomain* pd,
        CompletionQueue* send_cq, CompletionQueue* recv_cq,
        const ConnectionManagerConfig& config) {
    if (!pd || !send_cq || !recv_cq) {
        return Status::InvalidArgument("Invalid parameters");
    }

    auto manager = std::unique_ptr<ConnectionManager>(new ConnectionManager());
    manager->pd_ = pd;
    manager->send_cq_ = send_cq;
    manager->recv_cq_ = recv_cq;
    manager->config_ = config;

    // Pre-allocate connection slots
    manager->connections_.resize(config.max_connections);

    // Initialize free list
    manager->free_conn_ids_.reserve(config.max_connections);
    for (uint16_t i = 0; i < config.max_connections; ++i) {
        manager->free_conn_ids_.push_back(config.max_connections - 1 - i);
    }

    RDMA_LOG_INFO("Created connection manager with max %u connections", config.max_connections);

    return manager;
}

Result<Connection*> ConnectionManager::CreateConnection(const ConnectionConfig& config) {
    uint16_t conn_id = AllocateConnId();
    if (conn_id == UINT16_MAX) {
        return Status::ResourceExhausted("No available connection slots");
    }

    auto conn_result = Connection::Create(pd_, send_cq_, recv_cq_, config);
    if (!conn_result.ok()) {
        FreeConnId(conn_id);
        return conn_result.status();
    }

    auto& conn = conn_result.value();
    conn->SetConnId(conn_id);

    Connection* conn_ptr = conn.get();
    connections_[conn_id] = std::move(conn);
    ++active_count_;

    RDMA_LOG_DEBUG("Created connection %u", conn_id);

    return conn_ptr;
}

Connection* ConnectionManager::GetConnection(uint16_t conn_id) {
    if (conn_id >= connections_.size()) {
        return nullptr;
    }
    return connections_[conn_id].get();
}

void ConnectionManager::RemoveConnection(uint16_t conn_id) {
    if (conn_id >= connections_.size() || !connections_[conn_id]) {
        return;
    }

    connections_[conn_id].reset();
    FreeConnId(conn_id);
    --active_count_;

    RDMA_LOG_DEBUG("Removed connection %u", conn_id);
}

uint16_t ConnectionManager::AllocateConnId() {
    if (free_conn_ids_.empty()) {
        return UINT16_MAX;
    }
    uint16_t id = free_conn_ids_.back();
    free_conn_ids_.pop_back();
    return id;
}

void ConnectionManager::FreeConnId(uint16_t conn_id) {
    free_conn_ids_.push_back(conn_id);
}

}  // namespace rdma
