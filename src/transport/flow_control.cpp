#include "rdma/transport/flow_control.h"

namespace rdma {

FlowControl::FlowControl(const FlowControlConfig& config)
        : config_(config),
          lazy_threshold_(static_cast<uint32_t>(config.recv_depth * config.lazy_update_threshold)),
          current_threshold_ratio_(config.lazy_update_threshold) {
    send_credits_.store(config.recv_depth, std::memory_order_relaxed);
    pending_credits_.store(0, std::memory_order_relaxed);
    last_sample_time_ = std::chrono::steady_clock::now();
    last_activity_ = std::chrono::steady_clock::now();
}

void FlowControl::UpdateRateStatistics(uint32_t count) {
    window_recv_count_ += count;
    ++window_sample_count_;

    if (window_sample_count_ >= config_.window_size) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sample_time_)
                        .count();

        if (elapsed_ms > 0) {
            estimated_rate_ = static_cast<uint32_t>(window_recv_count_ * 1000 / elapsed_ms);
            AdjustThreshold();
        }

        window_recv_count_ = 0;
        window_sample_count_ = 0;
        last_sample_time_ = now;
    }
}

void FlowControl::AdjustThreshold() {
    float new_ratio;

    if (estimated_rate_ >= config_.high_rate_threshold) {
        // High throughput: increase threshold to reduce control packets
        new_ratio = config_.max_threshold;
    } else if (estimated_rate_ <= config_.low_rate_threshold) {
        // Low throughput/high latency: decrease threshold for smoothness
        new_ratio = config_.min_threshold;
    } else {
        // Medium load: linear interpolation
        float rate_ratio =
                static_cast<float>(estimated_rate_ - config_.low_rate_threshold) /
                (config_.high_rate_threshold - config_.low_rate_threshold);
        new_ratio = config_.min_threshold +
                    rate_ratio * (config_.max_threshold - config_.min_threshold);
    }

    // Smooth update (avoid jitter)
    current_threshold_ratio_ = 0.8f * current_threshold_ratio_ + 0.2f * new_ratio;
    lazy_threshold_ = static_cast<uint32_t>(config_.recv_depth * current_threshold_ratio_);
}

}  // namespace rdma
