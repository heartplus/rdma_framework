#pragma once

#include <memory>
#include <string>

#include "rdma/common/status.h"
#include "rdma/common/types.h"
#include "rdma/core/completion_queue.h"
#include "rdma/core/protection_domain.h"
#include "rdma/core/queue_pair.h"
#include "rdma/memory/buffer_pool.h"
#include "rdma/transport/flow_control.h"

namespace rdma {

// Forward declarations
class ConnectionManager;

// Connection configuration
struct ConnectionConfig {
    QueuePairConfig qp_config;
    FlowControlConfig flow_control_config;

    uint32_t recv_buffer_count = 256;
    size_t recv_buffer_size = 4096;

    uint32_t timeout_ms = 30000;
    uint32_t retry_count = 7;
    uint32_t rnr_retry = 7;

    uint8_t port_num = 1;
    int gid_index = 0;
};

// Connection class for managing RDMA connections
class Connection {
public:
    ~Connection();

    // Non-copyable
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Factory methods
    static Result<std::unique_ptr<Connection>> Create(ProtectionDomain* pd,
            CompletionQueue* send_cq, CompletionQueue* recv_cq,
            const ConnectionConfig& config = ConnectionConfig());

    // Connection ID
    uint16_t GetConnId() const { return conn_id_; }
    void SetConnId(uint16_t id) { conn_id_ = id; }

    // Connection state
    ConnectionState GetState() const { return state_; }

    // Get connection parameters for handshake
    Result<QpConnParams> GetLocalParams() const;

    // Connect to remote peer (after handshake)
    Status Connect(const QpConnParams& remote_params);

    // Disconnect
    Status Disconnect();

    // Send data
    Status Send(const Buffer& buf, uint64_t wr_id);
    Status SendInline(const void* data, size_t length, uint64_t wr_id);

    // RDMA Write
    Status Write(const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey, uint64_t wr_id);
    Status WriteWithImm(const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey,
            uint32_t imm_data, uint64_t wr_id);

    // RDMA Read
    Status Read(const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey, uint64_t wr_id);

    // Post receive buffer
    Status PostRecv(const Buffer& buf, uint64_t wr_id);

    // Flow control
    FlowControl* GetFlowControl() { return &flow_control_; }
    bool CanSend() const { return flow_control_.GetSendCredits() >= 1; }

    // Accessors
    QueuePair* GetQP() const { return qp_.get(); }
    ProtectionDomain* GetPD() const { return pd_; }
    const ConnectionConfig& GetConfig() const { return config_; }

    // Remote memory info (set during handshake)
    void SetRemoteRecvBuffer(const RemoteMemoryInfo& info) { remote_recv_buffer_ = info; }
    const RemoteMemoryInfo& GetRemoteRecvBuffer() const { return remote_recv_buffer_; }

    // Peer address (for debugging)
    void SetPeerAddress(const std::string& addr) { peer_address_ = addr; }
    const std::string& GetPeerAddress() const { return peer_address_; }

private:
    Connection() = default;

    std::unique_ptr<QueuePair> qp_;
    ProtectionDomain* pd_ = nullptr;
    CompletionQueue* send_cq_ = nullptr;
    CompletionQueue* recv_cq_ = nullptr;
    ConnectionConfig config_;

    uint16_t conn_id_ = 0;
    ConnectionState state_ = ConnectionState::DISCONNECTED;

    FlowControl flow_control_;

    RemoteMemoryInfo remote_recv_buffer_;
    std::string peer_address_;
};

}  // namespace rdma
