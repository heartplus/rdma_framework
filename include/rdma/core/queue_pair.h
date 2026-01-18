#pragma once

#include <infiniband/verbs.h>

#include <memory>

#include "rdma/common/status.h"
#include "rdma/common/types.h"
#include "rdma/core/completion_queue.h"
#include "rdma/core/protection_domain.h"
#include "rdma/memory/buffer.h"

namespace rdma {

// Queue Pair configuration
struct QueuePairConfig {
    uint32_t max_send_wr = 4096;
    uint32_t max_recv_wr = 4096;
    uint32_t max_send_sge = 16;
    uint32_t max_recv_sge = 16;
    uint32_t max_inline_data = 256;

    uint32_t signal_interval = 16;  // Signal every N WRs
    uint32_t cq_depth = 65536;      // CQ depth (must be large enough)

    ibv_qp_type qp_type = IBV_QPT_RC;  // RC for reliable connection
};

// Queue Pair wrapper with RAII
class QueuePair {
public:
    ~QueuePair();

    // Non-copyable
    QueuePair(const QueuePair&) = delete;
    QueuePair& operator=(const QueuePair&) = delete;

    // Moveable
    QueuePair(QueuePair&& other) noexcept;
    QueuePair& operator=(QueuePair&& other) noexcept;

    // Factory method
    static Result<std::unique_ptr<QueuePair>> Create(ProtectionDomain* pd,
            CompletionQueue* send_cq, CompletionQueue* recv_cq,
            const QueuePairConfig& config = QueuePairConfig());

    // QP state transitions
    Status TransitionToInit(uint8_t port_num = 1);
    Status TransitionToRTR(const QpConnParams& remote_params);
    Status TransitionToRTS();
    Status TransitionToError();
    Status Reset();

    // Get local connection parameters for handshake
    Result<QpConnParams> GetLocalParams(uint8_t port_num = 1, int gid_index = 0) const;

    // CQ Backpressure check
    ALWAYS_INLINE bool CanPostSignaled() const {
        return outstanding_signaled_ < (config_.cq_depth / 2);
    }

    uint32_t GetOutstandingSignaled() const { return outstanding_signaled_; }

    // Send operations
    HOT_FUNCTION Status PostSend(const Buffer& buf, uint64_t wr_id);
    Status PostSendInline(const void* data, size_t length, uint64_t wr_id);

    // RDMA Write operations
    Status PostWrite(const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey,
            uint64_t wr_id);
    Status PostWriteWithImm(const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey,
            uint32_t imm_data, uint64_t wr_id);

    // RDMA Read operations
    Status PostRead(
            const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey, uint64_t wr_id);

    // Receive operations
    Status PostRecv(const Buffer& buf, uint64_t wr_id);

    // Completion callback
    ALWAYS_INLINE void OnSendComplete(bool was_signaled) {
        if (was_signaled) {
            --outstanding_signaled_;
        }
    }

    // SGL operations with auto-fragmentation
    Status PostSendSGL(const struct iovec* iov, size_t iov_count, uint64_t wr_id_base,
            uint16_t conn_id, uint8_t generation);

    // Accessors
    ibv_qp* GetRawQP() const { return qp_; }
    uint32_t GetQpNum() const { return qp_ ? qp_->qp_num : 0; }
    uint32_t GetMaxSGE() const { return config_.max_send_sge; }
    const QueuePairConfig& GetConfig() const { return config_; }

private:
    QueuePair() = default;

    // Internal send with signaling decision
    Status DoPostSend(const Buffer& buf, uint64_t wr_id, bool signaled);
    Status DoPostSendSGL(const struct iovec* iov, size_t count, uint32_t lkey, uint64_t wr_id,
            bool signaled);

    // Determine if this WR should be signaled
    ALWAYS_INLINE bool ShouldSignal() {
        bool should_signal = (++unsignaled_count_ >= config_.signal_interval);
        if (should_signal) {
            if (!CanPostSignaled()) {
                return false;  // Backpressure
            }
            unsignaled_count_ = 0;
            ++outstanding_signaled_;
        }
        return should_signal;
    }

    ibv_qp* qp_ = nullptr;
    ProtectionDomain* pd_ = nullptr;
    CompletionQueue* send_cq_ = nullptr;
    CompletionQueue* recv_cq_ = nullptr;
    QueuePairConfig config_;

    uint32_t unsignaled_count_ = 0;
    uint32_t outstanding_signaled_ = 0;
};

}  // namespace rdma
