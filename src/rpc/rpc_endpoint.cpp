#include "rdma/rpc/rpc_endpoint.h"

#include <arpa/inet.h>

#include "rdma/common/logging.h"
#include "rdma/common/wr_id.h"

namespace rdma {

RpcEndpoint::~RpcEndpoint() {}

Result<std::unique_ptr<RpcEndpoint>> RpcEndpoint::Create(Connection* conn,
        LocalBufferPool* buffer_pool, TimingWheel* timing_wheel) {
    return Create(conn, buffer_pool, timing_wheel, Config{});
}

Result<std::unique_ptr<RpcEndpoint>> RpcEndpoint::Create(Connection* conn,
        LocalBufferPool* buffer_pool, TimingWheel* timing_wheel, const Config& config) {
    if (!conn || !buffer_pool || !timing_wheel) {
        return Status::InvalidArgument("Invalid parameters");
    }

    auto endpoint = std::unique_ptr<RpcEndpoint>(new RpcEndpoint());
    endpoint->conn_ = conn;
    endpoint->buffer_pool_ = buffer_pool;
    endpoint->timing_wheel_ = timing_wheel;
    endpoint->config_ = config;

    RDMA_LOG_DEBUG("Created RPC endpoint for connection %u", conn->GetConnId());

    return endpoint;
}

Result<uint32_t> RpcEndpoint::SendRequest(const void* header, size_t header_len,
        const Buffer* data_buf, void* callback_ctx, RpcCallback callback) {
    // 1. Allocate slot
    auto [slot_index, generation] = slot_pool_.Allocate();
    if (slot_index == UINT32_MAX) {
        return Status::ResourceExhausted("No available RPC slots");
    }

    RpcSlotHot* hot = slot_pool_.GetHot(slot_index);
    RpcSlotContext* context = slot_pool_.GetContext(slot_index);

    // 2. Initialize slot
    hot->SetState(RpcSlotHot::kStateInUse);
    hot->conn_id = conn_->GetConnId();
    hot->total_wr_count = 1;
    hot->completed_wr_count = 0;
    context->callback_ctx = callback_ctx;
    context->callback_fn = callback;

    // 3. Register timeout (timing wheel, O(1))
    timing_wheel_->Schedule(slot_index, generation, config_.timeout_ms);

    // 4. Send header (forced inline)
    uint64_t wr_id = WrId::Encode(conn_->GetConnId(), RpcOpcode::REQUEST_SEND, generation,
            slot_index);

    auto status = conn_->SendInline(header, header_len, wr_id);
    if (!status.ok()) {
        slot_pool_.Free(slot_index);
        return status;
    }

    // 5. If there's large data, use WriteWithImm
    if (data_buf && data_buf->IsValid()) {
        hot->total_wr_count++;

        uint32_t imm_data = (static_cast<uint32_t>(generation) << 16) | (slot_index & 0xFFFF);

        auto& remote_buf = conn_->GetRemoteRecvBuffer();
        status = conn_->WriteWithImm(*data_buf, remote_buf.addr, remote_buf.rkey, imm_data, wr_id);
        if (!status.ok()) {
            // Note: first WR already sent, connection may be in inconsistent state
            RDMA_LOG_ERROR("Failed to send data WriteWithImm");
        }
    }

    return slot_index;
}

Result<uint32_t> RpcEndpoint::SendRequestSGL(const void* header, size_t header_len,
        const struct iovec* iov, size_t iov_count, void* callback_ctx, RpcCallback callback) {
    // For SGL requests, combine header and data
    auto [slot_index, generation] = slot_pool_.Allocate();
    if (slot_index == UINT32_MAX) {
        return Status::ResourceExhausted("No available RPC slots");
    }

    RpcSlotHot* hot = slot_pool_.GetHot(slot_index);
    RpcSlotContext* context = slot_pool_.GetContext(slot_index);

    hot->SetState(RpcSlotHot::kStateInUse);
    hot->conn_id = conn_->GetConnId();
    hot->total_wr_count = 1;
    hot->completed_wr_count = 0;
    context->callback_ctx = callback_ctx;
    context->callback_fn = callback;

    timing_wheel_->Schedule(slot_index, generation, config_.timeout_ms);

    // First send header inline
    uint64_t wr_id = WrId::Encode(conn_->GetConnId(), RpcOpcode::REQUEST_SEND, generation,
            slot_index);

    auto status = conn_->SendInline(header, header_len, wr_id);
    if (!status.ok()) {
        slot_pool_.Free(slot_index);
        return status;
    }

    // Then send SGL data if present
    if (iov && iov_count > 0) {
        hot->total_wr_count++;
        status = conn_->GetQP()->PostSendSGL(iov, iov_count, slot_index, conn_->GetConnId(),
                generation);
        if (!status.ok()) {
            RDMA_LOG_ERROR("Failed to send SGL data");
        }
    }

    return slot_index;
}

Status RpcEndpoint::SendReadResponse(uint32_t client_slot_index, uint8_t client_generation,
        const Buffer& data_buf, const RemoteMemoryInfo& client_buffer) {
    // Encode imm_data
    uint32_t imm_data = (static_cast<uint32_t>(client_generation) << 16) |
                        (client_slot_index & 0xFFFF);

    // Directly write to client buffer, no header needed
    return conn_->WriteWithImm(data_buf, client_buffer.addr, client_buffer.rkey,
            htonl(imm_data), 0);
}

int RpcEndpoint::CheckTimeouts(std::vector<std::pair<uint32_t, uint8_t>>& expired) {
    int count = 0;

    for (const auto& entry : expired) {
        RpcSlotHot* hot = slot_pool_.GetHotChecked(entry.first, entry.second);
        if (!hot || hot->IsCompleted()) continue;

        // Mark as timeout
        hot->error_code = 1;  // timeout
        hot->completed_wr_count = hot->total_wr_count;

        // Get context and invoke callback
        RpcSlotContext* context = slot_pool_.GetContext(entry.first);
        if (context->callback_fn) {
            context->callback_fn(context->callback_ctx, entry.first, 1);  // 1 = timeout
        }

        slot_pool_.Free(entry.first);
        ++count;
    }

    return count;
}

void RpcEndpoint::OnRpcComplete(uint32_t slot_index) {
    RpcSlotContext* context = slot_pool_.GetContext(slot_index);
    RpcSlotHot* hot = slot_pool_.GetHot(slot_index);

    if (context->callback_fn) {
        context->callback_fn(context->callback_ctx, slot_index, hot->error_code);
    }

    slot_pool_.Free(slot_index);
}

void RpcEndpoint::RepostRecvBuffer() {
    Buffer buf = buffer_pool_->Allocate();
    if (buf.IsValid()) {
        conn_->PostRecv(buf, 0);
    }
}

}  // namespace rdma
