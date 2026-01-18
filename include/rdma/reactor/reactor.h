#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "rdma/common/mpsc_queue.h"
#include "rdma/common/status.h"
#include "rdma/common/timing_wheel.h"
#include "rdma/common/types.h"
#include "rdma/core/completion_queue.h"
#include "rdma/rpc/completion_dispatcher.h"

#ifdef __x86_64__
#include <emmintrin.h>
#endif

namespace rdma {

// Reactor configuration
struct ReactorConfig {
    BatchMode batch_mode = BatchMode::THROUGHPUT;

    int latency_batch_size = 16;
    int throughput_batch_size = 64;
    int max_batch_size = 128;

    uint32_t max_spin_count = 1000;

    // NUMA configuration
    int cpu_core = -1;  // -1 = no binding
    int numa_node = -1;
};

// Reactor for event loop
// Design considerations:
// 1. CQ batch size adjustable: latency-optimized 8-16, throughput-optimized 32-64
// 2. MPSC queue batch consumption: one exchange takes entire list
// 3. Core loop has __attribute__((hot))
// 4. Logs can be removed at compile time for benchmarks/prod
class Reactor {
public:
    // Poller function type
    using Poller = int (*)(void* ctx);

    ~Reactor();

    // Non-copyable
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    // Factory method
    static Result<std::unique_ptr<Reactor>> Create(const ReactorConfig& config = ReactorConfig());

    // Must be called in Reactor thread
    Status Initialize();

    // Register CQ with dispatcher
    void RegisterCQ(CompletionQueue* cq, CompletionDispatcher* dispatcher);

    // Register custom poller
    void RegisterPoller(Poller fn, void* ctx);

    // Schedule timeout
    void ScheduleTimeout(uint32_t slot_index, uint8_t generation, uint32_t timeout_ms);

    // Core poll loop
    HOT_FUNCTION int Poll();

    // Run event loop
    void Run();

    // Stop event loop
    void Stop();

    // Cross-thread task execution (lock-free)
    void Execute(Task* task);

    // Get current batch size
    int GetCurrentBatchSize() const { return current_batch_size_; }

    // Get timing wheel
    TimingWheel* GetTimingWheel() { return timing_wheel_.get(); }

    // Check if running
    bool IsRunning() const { return running_.load(std::memory_order_relaxed); }

    // Get thread ID
    std::thread::id GetThreadId() const { return thread_id_; }

private:
    Reactor() = default;

    struct CQEntry {
        CompletionQueue* cq;
        CompletionDispatcher* dispatcher;
    };

    struct PollerEntry {
        Poller fn;
        void* ctx;
    };

    ReactorConfig config_;
    std::atomic<bool> running_{false};
    std::thread::id thread_id_;

    std::vector<CQEntry> cq_entries_;
    std::vector<PollerEntry> pollers_;

    std::unique_ptr<TimingWheel> timing_wheel_;

    // MPSC queue (batch consumption)
    MpscQueue<Task> task_queue_;

    // Pre-allocated WC buffer
    ibv_wc* wc_buffer_ = nullptr;
    int wc_buffer_size_ = 0;

    // Adaptive batch size
    int current_batch_size_ = 32;
    uint64_t poll_count_ = 0;
    uint64_t total_wc_count_ = 0;

    // Idle spin backoff
    uint32_t idle_spin_count_ = 0;

    // Process cross-thread tasks
    HOT_FUNCTION int ProcessTasks();

    // Process timeouts
    HOT_FUNCTION int ProcessTimeouts();

    // Adapt batch size
    void AdaptBatchSize(int wc_count);

    // Adaptive backoff
    ALWAYS_INLINE void AdaptiveBackoff() {
        ++idle_spin_count_;

#ifdef __x86_64__
        if (idle_spin_count_ < 100) {
            _mm_pause();
        } else if (idle_spin_count_ < 1000) {
            for (int i = 0; i < 4; ++i) {
                _mm_pause();
            }
        } else if (idle_spin_count_ < config_.max_spin_count) {
            for (int i = 0; i < 16; ++i) {
                _mm_pause();
            }
        } else {
            idle_spin_count_ = config_.max_spin_count;
            for (int i = 0; i < 32; ++i) {
                _mm_pause();
            }
        }
#else
        // ARM or other: use yield
        if (idle_spin_count_ >= config_.max_spin_count) {
            std::this_thread::yield();
            idle_spin_count_ = config_.max_spin_count;
        }
#endif
    }

    // Pin thread to CPU
    static Status PinThread(int cpu_core);
};

}  // namespace rdma
