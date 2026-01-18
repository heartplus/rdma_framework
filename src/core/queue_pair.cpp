#include "rdma/core/queue_pair.h"

#include <arpa/inet.h>
#include <cstring>

#include "rdma/common/logging.h"
#include "rdma/common/wr_id.h"

namespace rdma {

QueuePair::~QueuePair() {
    if (qp_) {
        ibv_destroy_qp(qp_);
        qp_ = nullptr;
    }
}

QueuePair::QueuePair(QueuePair&& other) noexcept
        : qp_(other.qp_),
          pd_(other.pd_),
          send_cq_(other.send_cq_),
          recv_cq_(other.recv_cq_),
          config_(other.config_),
          unsignaled_count_(other.unsignaled_count_),
          outstanding_signaled_(other.outstanding_signaled_) {
    other.qp_ = nullptr;
    other.pd_ = nullptr;
    other.send_cq_ = nullptr;
    other.recv_cq_ = nullptr;
}

QueuePair& QueuePair::operator=(QueuePair&& other) noexcept {
    if (this != &other) {
        if (qp_) {
            ibv_destroy_qp(qp_);
        }
        qp_ = other.qp_;
        pd_ = other.pd_;
        send_cq_ = other.send_cq_;
        recv_cq_ = other.recv_cq_;
        config_ = other.config_;
        unsignaled_count_ = other.unsignaled_count_;
        outstanding_signaled_ = other.outstanding_signaled_;
        other.qp_ = nullptr;
        other.pd_ = nullptr;
        other.send_cq_ = nullptr;
        other.recv_cq_ = nullptr;
    }
    return *this;
}

Result<std::unique_ptr<QueuePair>> QueuePair::Create(ProtectionDomain* pd,
        CompletionQueue* send_cq, CompletionQueue* recv_cq, const QueuePairConfig& config) {
    if (!pd || !pd->GetRawPD()) {
        return Status::InvalidArgument("Invalid protection domain");
    }
    if (!send_cq || !send_cq->GetRawCQ()) {
        return Status::InvalidArgument("Invalid send completion queue");
    }
    if (!recv_cq || !recv_cq->GetRawCQ()) {
        return Status::InvalidArgument("Invalid receive completion queue");
    }

    ibv_qp_init_attr init_attr = {};
    init_attr.send_cq = send_cq->GetRawCQ();
    init_attr.recv_cq = recv_cq->GetRawCQ();
    init_attr.qp_type = config.qp_type;
    init_attr.cap.max_send_wr = config.max_send_wr;
    init_attr.cap.max_recv_wr = config.max_recv_wr;
    init_attr.cap.max_send_sge = config.max_send_sge;
    init_attr.cap.max_recv_sge = config.max_recv_sge;
    init_attr.cap.max_inline_data = config.max_inline_data;
    init_attr.sq_sig_all = 0;  // Manual signaling

    ibv_qp* qp = ibv_create_qp(pd->GetRawPD(), &init_attr);
    if (!qp) {
        return Status::IOError("Failed to create queue pair", errno);
    }

    auto pair = std::unique_ptr<QueuePair>(new QueuePair());
    pair->qp_ = qp;
    pair->pd_ = pd;
    pair->send_cq_ = send_cq;
    pair->recv_cq_ = recv_cq;
    pair->config_ = config;

    RDMA_LOG_DEBUG("Created queue pair: qp_num=%u", qp->qp_num);

    return pair;
}

Status QueuePair::TransitionToInit(uint8_t port_num) {
    ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = port_num;
    attr.qp_access_flags =
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

    if (ibv_modify_qp(qp_, &attr, flags) != 0) {
        return Status::IOError("Failed to transition QP to INIT", errno);
    }

    RDMA_LOG_DEBUG("QP %u transitioned to INIT", qp_->qp_num);
    return Status::OK();
}

Status QueuePair::TransitionToRTR(const QpConnParams& remote_params) {
    ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = static_cast<ibv_mtu>(remote_params.mtu);
    attr.dest_qp_num = remote_params.qp_num;
    attr.rq_psn = remote_params.psn;
    attr.max_dest_rd_atomic = 16;
    attr.min_rnr_timer = 12;

    attr.ah_attr.dlid = remote_params.lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = remote_params.port_num;

    // Check if using RoCE (GID is not all zeros)
    bool use_grh = false;
    for (int i = 0; i < 16; ++i) {
        if (remote_params.gid[i] != 0) {
            use_grh = true;
            break;
        }
    }

    if (use_grh) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.hop_limit = 64;
        attr.ah_attr.grh.dgid = *reinterpret_cast<const ibv_gid*>(remote_params.gid);
        attr.ah_attr.grh.sgid_index = remote_params.gid_index;
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.traffic_class = 0;
    }

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    if (ibv_modify_qp(qp_, &attr, flags) != 0) {
        return Status::IOError("Failed to transition QP to RTR", errno);
    }

    RDMA_LOG_DEBUG("QP %u transitioned to RTR", qp_->qp_num);
    return Status::OK();
}

Status QueuePair::TransitionToRTS() {
    ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 16;

    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    if (ibv_modify_qp(qp_, &attr, flags) != 0) {
        return Status::IOError("Failed to transition QP to RTS", errno);
    }

    RDMA_LOG_DEBUG("QP %u transitioned to RTS", qp_->qp_num);
    return Status::OK();
}

Status QueuePair::TransitionToError() {
    ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_ERR;

    if (ibv_modify_qp(qp_, &attr, IBV_QP_STATE) != 0) {
        return Status::IOError("Failed to transition QP to ERROR", errno);
    }

    return Status::OK();
}

Status QueuePair::Reset() {
    ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RESET;

    if (ibv_modify_qp(qp_, &attr, IBV_QP_STATE) != 0) {
        return Status::IOError("Failed to reset QP", errno);
    }

    unsignaled_count_ = 0;
    outstanding_signaled_ = 0;

    return Status::OK();
}

Result<QpConnParams> QueuePair::GetLocalParams(uint8_t port_num, int gid_index) const {
    QpConnParams params = {};
    params.qp_num = qp_->qp_num;
    params.psn = 0;
    params.port_num = port_num;
    params.gid_index = gid_index;

    // Get port attributes for LID
    auto port_result = pd_->GetDevice()->QueryPort(port_num);
    if (!port_result.ok()) {
        return port_result.status();
    }
    params.lid = port_result.value().lid;
    params.mtu = port_result.value().active_mtu;

    // Get GID for RoCE
    auto gid_result = pd_->GetDevice()->QueryGid(port_num, gid_index);
    if (!gid_result.ok()) {
        return gid_result.status();
    }
    memcpy(params.gid, &gid_result.value(), sizeof(params.gid));

    return params;
}

Status QueuePair::PostSend(const Buffer& buf, uint64_t wr_id) {
    return DoPostSend(buf, wr_id, ShouldSignal());
}

Status QueuePair::DoPostSend(const Buffer& buf, uint64_t wr_id, bool signaled) {
    ibv_sge sge = {};
    sge.addr = reinterpret_cast<uint64_t>(buf.GetAddr());
    sge.length = static_cast<uint32_t>(buf.GetLength());
    sge.lkey = buf.GetLkey();

    ibv_send_wr wr = {};
    wr.wr_id = wr_id;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = signaled ? IBV_SEND_SIGNALED : 0;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(qp_, &wr, &bad_wr) != 0) {
        return Status::IOError("ibv_post_send failed", errno);
    }

    return Status::OK();
}

Status QueuePair::PostSendInline(const void* data, size_t length, uint64_t wr_id) {
    if (length > config_.max_inline_data) {
        return Status::InvalidArgument("Data too large for inline send");
    }

    ibv_sge sge = {};
    sge.addr = reinterpret_cast<uint64_t>(data);
    sge.length = static_cast<uint32_t>(length);

    ibv_send_wr wr = {};
    wr.wr_id = wr_id;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_INLINE;
    if (ShouldSignal()) {
        wr.send_flags |= IBV_SEND_SIGNALED;
    }
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(qp_, &wr, &bad_wr) != 0) {
        return Status::IOError("ibv_post_send inline failed", errno);
    }

    return Status::OK();
}

Status QueuePair::PostWrite(const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey,
        uint64_t wr_id) {
    ibv_sge sge = {};
    sge.addr = reinterpret_cast<uint64_t>(buf.GetAddr());
    sge.length = static_cast<uint32_t>(buf.GetLength());
    sge.lkey = buf.GetLkey();

    ibv_send_wr wr = {};
    wr.wr_id = wr_id;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = ShouldSignal() ? IBV_SEND_SIGNALED : 0;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = remote_rkey;

    ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(qp_, &wr, &bad_wr) != 0) {
        return Status::IOError("ibv_post_send RDMA write failed", errno);
    }

    return Status::OK();
}

Status QueuePair::PostWriteWithImm(const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey,
        uint32_t imm_data, uint64_t wr_id) {
    ibv_sge sge = {};
    sge.addr = reinterpret_cast<uint64_t>(buf.GetAddr());
    sge.length = static_cast<uint32_t>(buf.GetLength());
    sge.lkey = buf.GetLkey();

    ibv_send_wr wr = {};
    wr.wr_id = wr_id;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = ShouldSignal() ? IBV_SEND_SIGNALED : 0;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.imm_data = htonl(imm_data);
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = remote_rkey;

    ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(qp_, &wr, &bad_wr) != 0) {
        return Status::IOError("ibv_post_send RDMA write with imm failed", errno);
    }

    return Status::OK();
}

Status QueuePair::PostRead(const Buffer& buf, uint64_t remote_addr, uint32_t remote_rkey,
        uint64_t wr_id) {
    ibv_sge sge = {};
    sge.addr = reinterpret_cast<uint64_t>(buf.GetAddr());
    sge.length = static_cast<uint32_t>(buf.GetLength());
    sge.lkey = buf.GetLkey();

    ibv_send_wr wr = {};
    wr.wr_id = wr_id;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.send_flags = ShouldSignal() ? IBV_SEND_SIGNALED : 0;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = remote_rkey;

    ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(qp_, &wr, &bad_wr) != 0) {
        return Status::IOError("ibv_post_send RDMA read failed", errno);
    }

    return Status::OK();
}

Status QueuePair::PostRecv(const Buffer& buf, uint64_t wr_id) {
    ibv_sge sge = {};
    sge.addr = reinterpret_cast<uint64_t>(buf.GetAddr());
    sge.length = static_cast<uint32_t>(buf.GetLength());
    sge.lkey = buf.GetLkey();

    ibv_recv_wr wr = {};
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_recv_wr* bad_wr = nullptr;
    if (ibv_post_recv(qp_, &wr, &bad_wr) != 0) {
        return Status::IOError("ibv_post_recv failed", errno);
    }

    return Status::OK();
}

Status QueuePair::PostSendSGL(const struct iovec* iov, size_t iov_count, uint64_t wr_id_base,
        uint16_t conn_id, uint8_t generation) {
    const size_t max_sge = config_.max_send_sge;

    // Fast path: SGE count within limit
    if (LIKELY(iov_count <= max_sge)) {
        return DoPostSendSGL(iov, iov_count, 0, wr_id_base, ShouldSignal());
    }

    // Slow path: need fragmentation
    size_t remaining = iov_count;
    size_t offset = 0;
    uint32_t fragment_index = 0;

    while (remaining > 0) {
        size_t chunk = (remaining < max_sge) ? remaining : max_sge;
        bool is_last = (remaining == chunk);

        uint64_t frag_wr_id = WrId::Encode(conn_id, RpcOpcode::REQUEST_SEND, generation,
                (static_cast<uint32_t>(wr_id_base) & 0xFFFFFF) | (fragment_index << 24));

        auto status = DoPostSendSGL(iov + offset, chunk, 0, frag_wr_id, is_last && ShouldSignal());
        if (!status.ok()) return status;

        remaining -= chunk;
        offset += chunk;
        ++fragment_index;
    }

    return Status::OK();
}

Status QueuePair::DoPostSendSGL(const struct iovec* iov, size_t count, uint32_t lkey,
        uint64_t wr_id, bool signaled) {
    ibv_sge sges[16];
    if (count > 16) {
        return Status::InvalidArgument("Too many SGEs");
    }

    for (size_t i = 0; i < count; ++i) {
        sges[i].addr = reinterpret_cast<uint64_t>(iov[i].iov_base);
        sges[i].length = static_cast<uint32_t>(iov[i].iov_len);
        sges[i].lkey = lkey;
    }

    ibv_send_wr wr = {};
    wr.wr_id = wr_id;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = signaled ? IBV_SEND_SIGNALED : 0;
    wr.sg_list = sges;
    wr.num_sge = static_cast<int>(count);

    ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(qp_, &wr, &bad_wr) != 0) {
        return Status::IOError("ibv_post_send SGL failed", errno);
    }

    return Status::OK();
}

}  // namespace rdma
