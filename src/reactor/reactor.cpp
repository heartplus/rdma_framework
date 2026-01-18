#include "rdma/reactor/reactor.h"

#include <pthread.h>
#include <sched.h>

#include <cstdlib>

#include "rdma/common/logging.h"

namespace rdma {

Reactor::~Reactor() {
    Stop();
    if (wc_buffer_) {
        free(wc_buffer_);
        wc_buffer_ = nullptr;
    }
}

Result<std::unique_ptr<Reactor>> Reactor::Create(const ReactorConfig& config) {
    auto reactor = std::unique_ptr<Reactor>(new Reactor());
    reactor->config_ = config;

    // Initialize timing wheel
    reactor->timing_wheel_ = std::make_unique<TimingWheel>();

    // Determine batch size based on mode
    switch (config.batch_mode) {
        case BatchMode::LATENCY:
            reactor->current_batch_size_ = config.latency_batch_size;
            break;
        case BatchMode::THROUGHPUT:
            reactor->current_batch_size_ = config.throughput_batch_size;
            break;
        case BatchMode::ADAPTIVE:
            reactor->current_batch_size_ = 32;  // Start with medium
            break;
    }

    // Pre-allocate WC buffer
    reactor->wc_buffer_size_ = config.max_batch_size;
    reactor->wc_buffer_ = static_cast<ibv_wc*>(aligned_alloc(64,
            reactor->wc_buffer_size_ * sizeof(ibv_wc)));
    if (!reactor->wc_buffer_) {
        return Status::ResourceExhausted("Failed to allocate WC buffer");
    }

    RDMA_LOG_INFO("Created reactor with batch size %d", reactor->current_batch_size_);

    return reactor;
}

Status Reactor::Initialize() {
    thread_id_ = std::this_thread::get_id();

    // Pin thread if configured
    if (config_.cpu_core >= 0) {
        auto status = PinThread(config_.cpu_core);
        if (!status.ok()) {
            RDMA_LOG_WARN("Failed to pin thread to CPU %d: %s", config_.cpu_core,
                    status.message());
        }
    }

    return Status::OK();
}

void Reactor::RegisterCQ(CompletionQueue* cq, CompletionDispatcher* dispatcher) {
    cq_entries_.push_back({cq, dispatcher});
    RDMA_LOG_DEBUG("Registered CQ with dispatcher");
}

void Reactor::RegisterPoller(Poller fn, void* ctx) {
    pollers_.push_back({fn, ctx});
}

void Reactor::ScheduleTimeout(uint32_t slot_index, uint8_t generation, uint32_t timeout_ms) {
    timing_wheel_->Schedule(slot_index, generation, timeout_ms);
}

HOT_FUNCTION int Reactor::Poll() {
    int total_events = 0;

    // 1. CQ Polling (adaptive batch size)
    for (auto& entry : cq_entries_) {
        int n = entry.cq->Poll(wc_buffer_, current_batch_size_);
        if (n > 0) {
            entry.dispatcher->DispatchBatchSimple(wc_buffer_, n);
            total_events += n;
            total_wc_count_ += n;
        }
    }

    // 2. Process cross-thread tasks (batch consumption)
    total_events += ProcessTasks();

    // 3. Timing wheel timeout check
    total_events += ProcessTimeouts();

    // 4. Custom pollers
    for (auto& p : pollers_) {
        total_events += p.fn(p.ctx);
    }

    // 5. Adaptive adjustment
    if (++poll_count_ % 1000 == 0) {
        AdaptBatchSize(total_events);
    }

    // 6. Idle backoff
    if (total_events > 0) {
        idle_spin_count_ = 0;
    } else {
        AdaptiveBackoff();
    }

    return total_events;
}

void Reactor::Run() {
    running_.store(true, std::memory_order_release);

    while (running_.load(std::memory_order_relaxed)) {
        Poll();
    }
}

void Reactor::Stop() {
    running_.store(false, std::memory_order_release);
}

void Reactor::Execute(Task* task) {
    task_queue_.Push(task);
}

HOT_FUNCTION int Reactor::ProcessTasks() {
    // One exchange takes entire list
    Task* head = task_queue_.PopAll();
    if (!head) return 0;

    int count = 0;
    while (head) {
        Task* next = head->next;
        head->fn(head->ctx);
        // Note: Task lifecycle managed by caller (intrusive)
        head = next;
        ++count;
    }
    return count;
}

HOT_FUNCTION int Reactor::ProcessTimeouts() {
    static thread_local std::vector<std::pair<uint32_t, uint8_t>> expired;
    timing_wheel_->Tick(expired);
    return static_cast<int>(expired.size());
}

void Reactor::AdaptBatchSize(int wc_count) {
    if (config_.batch_mode != BatchMode::ADAPTIVE) {
        return;
    }

    // Calculate average WC per poll
    float avg_wc = static_cast<float>(total_wc_count_) / poll_count_;

    if (avg_wc > current_batch_size_ * 0.8f) {
        // High load: increase batch size
        current_batch_size_ = std::min(current_batch_size_ * 2, config_.max_batch_size);
    } else if (avg_wc < current_batch_size_ * 0.2f && current_batch_size_ > config_.latency_batch_size) {
        // Low load: decrease batch size
        current_batch_size_ = std::max(current_batch_size_ / 2, config_.latency_batch_size);
    }

    // Reset stats
    total_wc_count_ = 0;
    poll_count_ = 0;
}

Status Reactor::PinThread(int cpu_core) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_core, &cpuset);

    int ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (ret != 0) {
        return Status::IOError("Failed to set CPU affinity", ret);
    }

    RDMA_LOG_INFO("Thread pinned to CPU %d", cpu_core);
    return Status::OK();
#else
    (void)cpu_core;
    return Status::OK();
#endif
}

}  // namespace rdma
