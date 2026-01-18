#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "rdma/common/types.h"

namespace rdma {

// Lock-free Multi-Producer Single-Consumer Queue
// Design considerations:
// 1. Multiple producers can push concurrently
// 2. Single consumer (Reactor thread) consumes
// 3. Batch consumption: one exchange takes entire list
// 4. Uses intrusive linked list to avoid allocation

template <typename T>
class MpscQueue {
public:
    // T must have a 'next' pointer member
    static_assert(std::is_pointer_v<decltype(std::declval<T>().next)>,
            "T must have a 'next' pointer member");

    MpscQueue() : head_(nullptr) {}

    // Push a node (can be called from any thread)
    // Node ownership is transferred to the queue
    void Push(T* node) {
        T* old_head = head_.load(std::memory_order_relaxed);
        do {
            node->next = old_head;
        } while (!head_.compare_exchange_weak(
                old_head, node, std::memory_order_release, std::memory_order_relaxed));
    }

    // Pop all nodes at once (must be called from single consumer thread)
    // Returns head of linked list, caller owns all nodes
    T* PopAll() {
        return head_.exchange(nullptr, std::memory_order_acquire);
    }

    // Check if queue is empty (may be racy)
    bool Empty() const {
        return head_.load(std::memory_order_relaxed) == nullptr;
    }

private:
    std::atomic<T*> head_;
};

// Intrusive task for cross-thread execution
struct Task {
    void (*fn)(void* ctx);
    void* ctx;
    Task* next;

    Task() : fn(nullptr), ctx(nullptr), next(nullptr) {}
    Task(void (*f)(void*), void* c) : fn(f), ctx(c), next(nullptr) {}
};

// Fixed-size ring buffer for single-producer single-consumer
template <typename T, size_t Capacity>
class SpscRingBuffer {
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    SpscRingBuffer() : head_(0), tail_(0) {}

    // Push (producer thread only)
    bool Push(const T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) & (Capacity - 1);

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Full
        }

        buffer_[head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // Pop (consumer thread only)
    bool Pop(T& item) {
        size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return false;  // Empty
        }

        item = buffer_[tail];
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    bool Empty() const {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

    size_t Size() const {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_relaxed);
        return (head - tail + Capacity) & (Capacity - 1);
    }

private:
    alignas(kCacheLineSize) std::atomic<size_t> head_;
    alignas(kCacheLineSize) std::atomic<size_t> tail_;
    T buffer_[Capacity];
};

}  // namespace rdma
