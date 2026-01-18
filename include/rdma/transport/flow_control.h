#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "rdma/common/types.h"

namespace rdma {

// Flow Control configuration
struct FlowControlConfig {
    uint32_t recv_depth = 1024;
    float lazy_update_threshold = 0.25f;
    std::chrono::milliseconds idle_interval{100};

    // Adaptive threshold configuration
    bool enable_adaptive_threshold = true;
    float min_threshold = 0.10f;
    float max_threshold = 0.50f;
    uint32_t window_size = 100;
    uint32_t high_rate_threshold = 10000;
    uint32_t low_rate_threshold = 1000;
};

// Flow Control for credit-based flow control
// Design considerations:
// 1. Lazy update: only update when pending_credits > threshold% recv_depth
// 2. Piggyback: prefer to piggyback credits on responses
// 3. Use relaxed atomics (single-threaded consumption)
// 4. Adaptive threshold based on receive rate
class FlowControl {
public:
    explicit FlowControl(const FlowControlConfig& config = FlowControlConfig());

    // Sender: check and consume credit
    ALWAYS_INLINE bool TryConsume(uint32_t count = 1) {
        uint32_t current = send_credits_.load(std::memory_order_relaxed);
        if (current < count) return false;
        send_credits_.store(current - count, std::memory_order_relaxed);
        return true;
    }

    // Receiver: called after Recv completion
    ALWAYS_INLINE void OnRecvCompleted(uint32_t count = 1) {
        pending_credits_.fetch_add(count, std::memory_order_relaxed);

        if (config_.enable_adaptive_threshold) {
            UpdateRateStatistics(count);
        }
    }

    // Lazy update: check if credit update should be sent
    ALWAYS_INLINE bool ShouldUpdateCredit() const {
        return pending_credits_.load(std::memory_order_relaxed) >= lazy_threshold_;
    }

    // Get pending credits for piggyback
    uint32_t GetPiggybackCredits() {
        return pending_credits_.exchange(0, std::memory_order_relaxed);
    }

    // Receiver: add credits (called when credit update received)
    void AddCredits(uint32_t count) {
        send_credits_.fetch_add(count, std::memory_order_relaxed);
    }

    // Get current threshold ratio (for monitoring)
    float GetCurrentThresholdRatio() const { return current_threshold_ratio_; }

    // Get estimated receive rate (calls/sec)
    uint32_t GetEstimatedRecvRate() const { return estimated_rate_; }

    // Get available send credits
    uint32_t GetSendCredits() const { return send_credits_.load(std::memory_order_relaxed); }

    // Get pending credits
    uint32_t GetPendingCredits() const {
        return pending_credits_.load(std::memory_order_relaxed);
    }

    // Reset with new config (used during initialization)
    void Reset(const FlowControlConfig& config) {
        config_ = config;
        lazy_threshold_ = static_cast<uint32_t>(config.recv_depth * config.lazy_update_threshold);
        current_threshold_ratio_ = config.lazy_update_threshold;
        send_credits_.store(config.recv_depth, std::memory_order_relaxed);
        pending_credits_.store(0, std::memory_order_relaxed);
        last_activity_ = std::chrono::steady_clock::now();
        last_sample_time_ = last_activity_;
        window_recv_count_ = 0;
        window_sample_count_ = 0;
        estimated_rate_ = 0;
    }

private:
    void UpdateRateStatistics(uint32_t count);
    void AdjustThreshold();

    FlowControlConfig config_;
    uint32_t lazy_threshold_;
    float current_threshold_ratio_;

    std::atomic<uint32_t> send_credits_;
    std::atomic<uint32_t> pending_credits_;
    std::chrono::steady_clock::time_point last_activity_;

    // Adaptive threshold: sliding window statistics
    std::chrono::steady_clock::time_point last_sample_time_;
    uint32_t window_recv_count_ = 0;
    uint32_t window_sample_count_ = 0;
    uint32_t estimated_rate_ = 0;
};

}  // namespace rdma
