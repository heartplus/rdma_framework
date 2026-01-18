#pragma once

#include <arpa/inet.h>

#include <memory>

#include "rdma/common/status.h"
#include "rdma/common/timing_wheel.h"
#include "rdma/common/wr_id.h"
#include "rdma/memory/buffer.h"
#include "rdma/memory/buffer_pool.h"
#include "rdma/rpc/rpc_slot.h"
#include "rdma/transport/connection.h"

namespace rdma {

// RPC callback function type
using RpcCallback = void (*)(void* ctx, uint32_t slot_index, uint8_t status);

// RPC Endpoint for sending and receiving RPC messages
// Design considerations:
// 1. Header forced inline: saves NIC memory read
// 2. Data uses WriteWithImm: peer gets slot_index from CQE directly
// 3. Timeout uses timing wheel: O(1) insert and check
// 4. All hot path functions are inline
// 5. Zero-copy optimization: Read Response uses RDMA_WRITE_WITH_IMM
class RpcEndpoint {
public:
    struct Config {
        uint32_t timeout_ms = 30000;
        size_t max_inline_data = 256;
        bool enable_zero_copy_response = true;
    };

    ~RpcEndpoint();

    // Non-copyable
    RpcEndpoint(const RpcEndpoint&) = delete;
    RpcEndpoint& operator=(const RpcEndpoint&) = delete;

    // Factory methods
    static Result<std::unique_ptr<RpcEndpoint>> Create(Connection* conn,
            LocalBufferPool* buffer_pool, TimingWheel* timing_wheel,
            const Config& config);
    static Result<std::unique_ptr<RpcEndpoint>> Create(Connection* conn,
            LocalBufferPool* buffer_pool, TimingWheel* timing_wheel);

    // Send request (header forced inline)
    // Important: data must come from buffer_pool, not stack!
    HOT_FUNCTION Result<uint32_t> SendRequest(const void* header, size_t header_len,
            const Buffer* data_buf, void* callback_ctx, RpcCallback callback);

    // Send request with SGL
    HOT_FUNCTION Result<uint32_t> SendRequestSGL(const void* header, size_t header_len,
            const struct iovec* iov, size_t iov_count, void* callback_ctx, RpcCallback callback);

    // WC handlers (all inline)
    HOT_FUNCTION ALWAYS_INLINE void OnSendComplete(const ibv_wc& wc) {
        uint32_t slot_index = WrId::GetSlotIndex(wc.wr_id);
        uint8_t generation = WrId::GetGeneration(wc.wr_id);

        RpcSlotHot* hot = slot_pool_.GetHotChecked(slot_index, generation);
        if (UNLIKELY(!hot)) return;  // stale

        hot->MarkCompleted();

        if (hot->IsCompleted()) {
            OnRpcComplete(slot_index);
        }
    }

    // Handle RDMA_WRITE_WITH_IMM completion (zero-copy response)
    HOT_FUNCTION ALWAYS_INLINE void OnWriteWithImmRecv(const ibv_wc& wc) {
        uint32_t imm_data = ntohl(wc.imm_data);

        uint32_t slot_index = imm_data & 0xFFFF;
        uint8_t generation = (imm_data >> 16) & 0xFF;

        RpcSlotHot* hot = slot_pool_.GetHotChecked(slot_index, generation);
        if (UNLIKELY(!hot)) return;

        hot->MarkCompleted();

        if (hot->IsCompleted()) {
            OnRpcComplete(slot_index);
        }

        RepostRecvBuffer();
    }

    // Server: send Read Response (zero-copy path)
    HOT_FUNCTION Status SendReadResponse(uint32_t client_slot_index, uint8_t client_generation,
            const Buffer& data_buf, const RemoteMemoryInfo& client_buffer);

    // Timeout check (called by Reactor)
    HOT_FUNCTION int CheckTimeouts(std::vector<std::pair<uint32_t, uint8_t>>& expired);

    // Get slot pool
    RpcSlotPool* GetSlotPool() { return &slot_pool_; }

    // Get connection
    Connection* GetConnection() const { return conn_; }

private:
    RpcEndpoint() = default;

    void OnRpcComplete(uint32_t slot_index);
    void RepostRecvBuffer();

    Connection* conn_ = nullptr;
    LocalBufferPool* buffer_pool_ = nullptr;
    TimingWheel* timing_wheel_ = nullptr;

    RpcSlotPool slot_pool_;
    Config config_;
};

}  // namespace rdma
