#pragma once

#include <infiniband/verbs.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "rdma/common/status.h"
#include "rdma/common/types.h"
#include "rdma/core/queue_pair.h"

namespace rdma {

// Doorbell Batcher for reducing MMIO overhead
// Design considerations:
// 1. ibv_post_send triggers MMIO (Doorbell), which is expensive
// 2. Strategy: collect batch of WRs, flush at end of Reactor loop
// 3. Dual trigger mechanism:
//    - Batch size trigger: flush when max_batch_size reached
//    - Time trigger: flush when max_delay_us exceeded
//    - Idle trigger: flush when idle for 2+ poll cycles
class DoorbellBatcher {
public:
    struct Config {
        size_t max_batch_size = 32;     // Max batch size (trigger 1)
        uint32_t max_delay_us = 10;     // Max delay in microseconds (trigger 2)
        size_t low_load_threshold = 4;  // Low load threshold (immediate flush)
    };

    explicit DoorbellBatcher(QueuePair* qp) : qp_(qp), config_() {}
    DoorbellBatcher(QueuePair* qp, const Config& config) : qp_(qp), config_(config) {}

    // Add to batch queue (don't submit immediately)
    ALWAYS_INLINE void AddSendWr(ibv_send_wr* wr) {
        if (send_tail_) {
            send_tail_->next = wr;
        } else {
            send_head_ = wr;
            first_wr_tsc_ = GetTsc();
        }
        send_tail_ = wr;
        wr->next = nullptr;
        ++send_count_;

        // Trigger 1: reached max batch size
        if (UNLIKELY(send_count_ >= config_.max_batch_size)) {
            FlushSend();
        }
    }

    // Dual trigger check (called in Reactor::Poll)
    HOT_FUNCTION ALWAYS_INLINE bool ShouldFlush() const {
        if (send_count_ == 0) return false;

        // Trigger 1: reached batch size
        if (send_count_ >= config_.max_batch_size) {
            return true;
        }

        // Trigger 2: exceeded max delay time
        uint64_t elapsed_tsc = GetTsc() - first_wr_tsc_;
        uint64_t max_tsc = config_.max_delay_us * tsc_per_us_;
        if (elapsed_tsc >= max_tsc) {
            return true;
        }

        // Trigger 3: low load - immediate flush
        if (idle_poll_count_ >= 2 && send_count_ > 0) {
            return true;
        }

        return false;
    }

    // Unified submit (single Doorbell)
    HOT_FUNCTION Status FlushSend() {
        if (!send_head_) return Status::OK();

        // Memory barrier to ensure WR construction is visible to hardware
        std::atomic_thread_fence(std::memory_order_release);

        ibv_send_wr* bad_wr = nullptr;
        int ret = ibv_post_send(qp_->GetRawQP(), send_head_, &bad_wr);

        // Reset
        send_head_ = nullptr;
        send_tail_ = nullptr;
        send_count_ = 0;
        idle_poll_count_ = 0;

        if (ret != 0) {
            return Status::IOError("ibv_post_send failed", ret);
        }
        return Status::OK();
    }

    // Called at end of Reactor Poll
    HOT_FUNCTION Status OnPollEnd(bool had_events) {
        if (!had_events) {
            ++idle_poll_count_;
        } else {
            idle_poll_count_ = 0;
        }

        if (ShouldFlush()) {
            return FlushSend();
        }
        return Status::OK();
    }

    size_t PendingSendCount() const { return send_count_; }

    // Calibrate TSC frequency (call once at initialization)
    static void CalibrateTsc() {
        auto start = std::chrono::steady_clock::now();
        uint64_t tsc_start = GetTsc();

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        uint64_t tsc_end = GetTsc();
        auto end = std::chrono::steady_clock::now();

        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        if (elapsed_us > 0) {
            tsc_per_us_ = (tsc_end - tsc_start) / elapsed_us;
        }
    }

private:
    ALWAYS_INLINE static uint64_t GetTsc() {
#if defined(__x86_64__)
        unsigned int lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
        uint64_t val;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
        return val;
#else
        return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
    }

    QueuePair* qp_;
    Config config_;

    ibv_send_wr* send_head_ = nullptr;
    ibv_send_wr* send_tail_ = nullptr;
    size_t send_count_ = 0;

    uint64_t first_wr_tsc_ = 0;
    uint32_t idle_poll_count_ = 0;

    static inline uint64_t tsc_per_us_ = 2400;  // Default 2.4GHz, needs calibration
};

}  // namespace rdma
