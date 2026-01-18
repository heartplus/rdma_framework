#include "rdma/transport/connection.h"

#include "rdma/common/logging.h"

namespace rdma {

Connection::~Connection() {
    if (state_ == ConnectionState::CONNECTED) {
        Disconnect();
    }
}

Result<std::unique_ptr<Connection>> Connection::Create(ProtectionDomain* pd,
        CompletionQueue* send_cq, CompletionQueue* recv_cq, const ConnectionConfig& config) {
    if (!pd) {
        return Status::InvalidArgument("Invalid protection domain");
    }

    // Create QP
    auto qp_result = QueuePair::Create(pd, send_cq, recv_cq, config.qp_config);
    if (!qp_result.ok()) {
        return qp_result.status();
    }

    auto conn = std::unique_ptr<Connection>(new Connection());
    conn->qp_ = std::move(qp_result.value());
    conn->pd_ = pd;
    conn->send_cq_ = send_cq;
    conn->recv_cq_ = recv_cq;
    conn->config_ = config;
    conn->flow_control_.Reset(config.flow_control_config);
    conn->state_ = ConnectionState::DISCONNECTED;

    RDMA_LOG_DEBUG("Created connection with QP %u", conn->qp_->GetQpNum());

    return conn;
}

Result<QpConnParams> Connection::GetLocalParams() const {
    return qp_->GetLocalParams(config_.port_num, config_.gid_index);
}

Status Connection::Connect(const QpConnParams& remote_params) {
    if (state_ != ConnectionState::DISCONNECTED) {
        return Status::InvalidArgument("Connection already established or in progress");
    }

    state_ = ConnectionState::CONNECTING;

    // Transition to INIT
    auto status = qp_->TransitionToInit(config_.port_num);
    if (!status.ok()) {
        state_ = ConnectionState::ERROR;
        return status;
    }

    // Transition to RTR
    status = qp_->TransitionToRTR(remote_params);
    if (!status.ok()) {
        state_ = ConnectionState::ERROR;
        return status;
    }

    // Transition to RTS
    status = qp_->TransitionToRTS();
    if (!status.ok()) {
        state_ = ConnectionState::ERROR;
        return status;
    }

    state_ = ConnectionState::CONNECTED;

    RDMA_LOG_INFO("Connection established with remote QP %u", remote_params.qp_num);

    return Status::OK();
}

Status Connection::Disconnect() {
    if (state_ != ConnectionState::CONNECTED) {
        return Status::OK();
    }

    state_ = ConnectionState::CLOSING;

    // Transition QP to error state to flush pending work
    auto status = qp_->TransitionToError();
    if (!status.ok()) {
        RDMA_LOG_WARN("Failed to transition QP to error state");
    }

    state_ = ConnectionState::DISCONNECTED;

    RDMA_LOG_INFO("Connection disconnected");

    return Status::OK();
}

Status Connection::Send(const Buffer& buf, uint64_t wr_id) {
    if (state_ != ConnectionState::CONNECTED) {
        return Status::InvalidArgument("Connection not established");
    }
    return qp_->PostSend(buf, wr_id);
}

Status Connection::SendInline(const void* data, size_t length, uint64_t wr_id) {
    if (state_ != ConnectionState::CONNECTED) {
        return Status::InvalidArgument("Connection not established");
    }
    return qp_->PostSendInline(data, length, wr_id);
}

Status Connection::Write(const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey,
        uint64_t wr_id) {
    if (state_ != ConnectionState::CONNECTED) {
        return Status::InvalidArgument("Connection not established");
    }
    return qp_->PostWrite(buf, remote_addr, remote_rkey, wr_id);
}

Status Connection::WriteWithImm(const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey,
        uint32_t imm_data, uint64_t wr_id) {
    if (state_ != ConnectionState::CONNECTED) {
        return Status::InvalidArgument("Connection not established");
    }
    return qp_->PostWriteWithImm(buf, remote_addr, remote_rkey, imm_data, wr_id);
}

Status Connection::Read(const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey,
        uint64_t wr_id) {
    if (state_ != ConnectionState::CONNECTED) {
        return Status::InvalidArgument("Connection not established");
    }
    return qp_->PostRead(buf, remote_addr, remote_rkey, wr_id);
}

Status Connection::PostRecv(const Buffer& buf, uint64_t wr_id) {
    return qp_->PostRecv(buf, wr_id);
}

}  // namespace rdma
