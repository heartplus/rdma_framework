#include "rdma/core/completion_queue.h"

#include "rdma/common/logging.h"

namespace rdma {

CompletionQueue::~CompletionQueue() {
    if (cq_) {
        ibv_destroy_cq(cq_);
        cq_ = nullptr;
    }
    if (channel_) {
        ibv_destroy_comp_channel(channel_);
        channel_ = nullptr;
    }
}

CompletionQueue::CompletionQueue(CompletionQueue&& other) noexcept
        : cq_(other.cq_),
          channel_(other.channel_),
          device_(other.device_),
          depth_(other.depth_),
          outstanding_(other.outstanding_) {
    other.cq_ = nullptr;
    other.channel_ = nullptr;
    other.device_ = nullptr;
}

CompletionQueue& CompletionQueue::operator=(CompletionQueue&& other) noexcept {
    if (this != &other) {
        if (cq_) {
            ibv_destroy_cq(cq_);
        }
        if (channel_) {
            ibv_destroy_comp_channel(channel_);
        }
        cq_ = other.cq_;
        channel_ = other.channel_;
        device_ = other.device_;
        depth_ = other.depth_;
        outstanding_ = other.outstanding_;
        other.cq_ = nullptr;
        other.channel_ = nullptr;
        other.device_ = nullptr;
    }
    return *this;
}

Result<std::unique_ptr<CompletionQueue>> CompletionQueue::Create(RdmaDevice* device,
        const CompletionQueueConfig& config) {
    if (!device || !device->GetContext()) {
        return Status::InvalidArgument("Invalid device");
    }

    ibv_comp_channel* channel = nullptr;
    if (config.use_channel) {
        channel = ibv_create_comp_channel(device->GetContext());
        if (!channel) {
            return Status::IOError("Failed to create completion channel", errno);
        }
    }

    ibv_cq* cq = ibv_create_cq(device->GetContext(), config.cq_depth, nullptr, channel,
            config.comp_vector);
    if (!cq) {
        if (channel) {
            ibv_destroy_comp_channel(channel);
        }
        return Status::IOError("Failed to create completion queue", errno);
    }

    auto queue = std::unique_ptr<CompletionQueue>(new CompletionQueue());
    queue->cq_ = cq;
    queue->channel_ = channel;
    queue->device_ = device;
    queue->depth_ = config.cq_depth;

    RDMA_LOG_DEBUG("Created completion queue: depth=%u", config.cq_depth);

    return queue;
}

Status CompletionQueue::RequestNotification(bool solicited_only) {
    if (ibv_req_notify_cq(cq_, solicited_only ? 1 : 0) != 0) {
        return Status::IOError("Failed to request CQ notification", errno);
    }
    return Status::OK();
}

Status CompletionQueue::AckEvents(unsigned int nevents) {
    ibv_ack_cq_events(cq_, nevents);
    return Status::OK();
}

}  // namespace rdma
