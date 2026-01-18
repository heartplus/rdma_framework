#pragma once

#include <infiniband/verbs.h>

#include <memory>

#include "rdma/common/status.h"
#include "rdma/common/types.h"
#include "rdma/core/device.h"

namespace rdma {

// Completion Queue configuration
struct CompletionQueueConfig {
    uint32_t cq_depth = 65536;  // CQ depth (increased for 1M QPS)
    int comp_vector = 0;        // Completion vector for interrupt affinity
    bool use_channel = false;   // Use completion channel for events
};

// Completion Queue wrapper with RAII
class CompletionQueue {
public:
    ~CompletionQueue();

    // Non-copyable
    CompletionQueue(const CompletionQueue&) = delete;
    CompletionQueue& operator=(const CompletionQueue&) = delete;

    // Moveable
    CompletionQueue(CompletionQueue&& other) noexcept;
    CompletionQueue& operator=(CompletionQueue&& other) noexcept;

    // Factory method
    static Result<std::unique_ptr<CompletionQueue>> Create(RdmaDevice* device,
            const CompletionQueueConfig& config = CompletionQueueConfig());

    // Poll for completions (hot path)
    HOT_FUNCTION ALWAYS_INLINE int Poll(ibv_wc* wc_array, int max_wc) {
        return ibv_poll_cq(cq_, max_wc, wc_array);
    }

    // Get number of outstanding completions
    int GetOutstanding() const { return outstanding_; }

    // Accessors
    ibv_cq* GetRawCQ() const { return cq_; }
    uint32_t GetDepth() const { return depth_; }
    RdmaDevice* GetDevice() const { return device_; }

    // For interrupt-based completion
    ibv_comp_channel* GetChannel() const { return channel_; }
    Status RequestNotification(bool solicited_only = false);
    Status AckEvents(unsigned int nevents);

private:
    CompletionQueue() = default;

    ibv_cq* cq_ = nullptr;
    ibv_comp_channel* channel_ = nullptr;
    RdmaDevice* device_ = nullptr;
    uint32_t depth_ = 0;
    int outstanding_ = 0;
};

}  // namespace rdma
