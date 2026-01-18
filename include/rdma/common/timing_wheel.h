#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

#include "rdma/common/types.h"

namespace rdma {

// Timeout entry for timing wheel
struct TimeoutEntry {
    uint32_t slot_index;
    uint8_t generation;
    uint8_t _padding[3];
    TimeoutEntry* next;

    TimeoutEntry() : slot_index(0), generation(0), _padding{0, 0, 0}, next(nullptr) {}
};

// Ring buffer bucket (no dynamic allocation)
class TimingWheelBucket {
public:
    ALWAYS_INLINE void Push(TimeoutEntry* entry) {
        entry->next = head_;
        head_ = entry;
        ++count_;
    }

    ALWAYS_INLINE TimeoutEntry* PopAll() {
        TimeoutEntry* result = head_;
        head_ = nullptr;
        count_ = 0;
        return result;
    }

    size_t Count() const { return count_; }

private:
    TimeoutEntry* head_ = nullptr;
    size_t count_ = 0;
};

// Hierarchical Timing Wheel
// Design considerations:
// 1. priority_queue push is O(log N), 10%+ overhead at 1M QPS
// 2. Timing wheel: insert O(1), check O(1)
// 3. Two-level wheel for precision:
//    - Level 1: 1ms precision, 256 slots (covers 256ms)
//    - Level 2: 256ms precision, 256 slots (covers ~65s)
// 4. Ring buffer + fixed-size array, no vector resizing
class HierarchicalTimingWheel {
public:
    static constexpr size_t kLevel1Size = 256;
    static constexpr size_t kLevel2Size = 256;
    static constexpr uint32_t kLevel1TickMs = 1;
    static constexpr uint32_t kLevel2TickMs = 256;
    static constexpr size_t kMaxEntries = 1 << 20;  // 1M entries

    HierarchicalTimingWheel() {
        // Preallocate entry pool
        entry_pool_ = static_cast<TimeoutEntry*>(
                aligned_alloc(kCacheLineSize, kMaxEntries * sizeof(TimeoutEntry)));

        // Initialize free list
        for (size_t i = 0; i < kMaxEntries - 1; ++i) {
            entry_pool_[i].next = &entry_pool_[i + 1];
        }
        entry_pool_[kMaxEntries - 1].next = nullptr;
        free_list_ = &entry_pool_[0];
    }

    ~HierarchicalTimingWheel() {
        free(entry_pool_);
    }

    // O(1) insert
    ALWAYS_INLINE bool Schedule(uint32_t slot_index, uint8_t generation, uint32_t timeout_ms) {
        TimeoutEntry* entry = AllocEntry();
        if (UNLIKELY(!entry)) return false;

        entry->slot_index = slot_index;
        entry->generation = generation;

        if (timeout_ms < kLevel1Size * kLevel1TickMs) {
            // Short timeout: directly insert into level 1
            uint32_t target = (level1_current_ + timeout_ms) % kLevel1Size;
            level1_[target].Push(entry);
        } else {
            // Long timeout: insert into level 2
            uint32_t level2_ticks = timeout_ms / kLevel2TickMs;
            uint32_t target = (level2_current_ + level2_ticks) % kLevel2Size;
            level2_[target].Push(entry);
        }
        return true;
    }

    // O(1) timeout check (called every 1ms)
    HOT_FUNCTION
    void Tick(std::vector<std::pair<uint32_t, uint8_t>>& expired) {
        expired.clear();

        // 1. Process current level 1 bucket
        TimeoutEntry* entry = level1_[level1_current_].PopAll();
        while (entry) {
            expired.emplace_back(entry->slot_index, entry->generation);
            TimeoutEntry* next = entry->next;
            FreeEntry(entry);
            entry = next;
        }

        // 2. Advance level 1
        level1_current_ = (level1_current_ + 1) % kLevel1Size;

        // 3. Every 256ms, demote level 2 entries
        if (level1_current_ == 0) {
            entry = level2_[level2_current_].PopAll();
            while (entry) {
                TimeoutEntry* next = entry->next;
                // Redistribute to level 1
                uint32_t target = (reinterpret_cast<uintptr_t>(entry) >> 6) % kLevel1Size;
                level1_[target].Push(entry);
                entry = next;
            }

            level2_current_ = (level2_current_ + 1) % kLevel2Size;
        }
    }

    // Cancel timeout by generation validation (implicit)
    // When generation doesn't match, expired handler will ignore

    size_t GetFreeCount() const {
        size_t count = 0;
        TimeoutEntry* entry = free_list_;
        while (entry) {
            ++count;
            entry = entry->next;
        }
        return count;
    }

private:
    TimingWheelBucket level1_[kLevel1Size];
    TimingWheelBucket level2_[kLevel2Size];

    uint32_t level1_current_ = 0;
    uint32_t level2_current_ = 0;

    TimeoutEntry* entry_pool_;
    TimeoutEntry* free_list_;

    ALWAYS_INLINE TimeoutEntry* AllocEntry() {
        if (UNLIKELY(!free_list_)) return nullptr;
        TimeoutEntry* entry = free_list_;
        free_list_ = entry->next;
        return entry;
    }

    ALWAYS_INLINE void FreeEntry(TimeoutEntry* entry) {
        entry->next = free_list_;
        free_list_ = entry;
    }
};

// Simple timing wheel (single level, for simpler use cases)
class TimingWheel {
public:
    static constexpr size_t kWheelSize = 1024;
    static constexpr size_t kMaxEntries = 65536;

    TimingWheel() {
        entry_pool_ =
                static_cast<TimeoutEntry*>(aligned_alloc(64, kMaxEntries * sizeof(TimeoutEntry)));

        for (size_t i = 0; i < kMaxEntries - 1; ++i) {
            entry_pool_[i].next = &entry_pool_[i + 1];
        }
        entry_pool_[kMaxEntries - 1].next = nullptr;
        free_list_ = &entry_pool_[0];
    }

    ~TimingWheel() {
        free(entry_pool_);
    }

    bool Schedule(uint32_t slot_index, uint8_t generation, uint32_t timeout_ticks) {
        TimeoutEntry* entry = AllocEntry();
        if (!entry) return false;

        entry->slot_index = slot_index;
        entry->generation = generation;

        uint32_t target = (current_ + timeout_ticks) % kWheelSize;
        buckets_[target].Push(entry);
        return true;
    }

    void Tick(std::vector<std::pair<uint32_t, uint8_t>>& expired) {
        expired.clear();

        TimeoutEntry* entry = buckets_[current_].PopAll();
        while (entry) {
            expired.emplace_back(entry->slot_index, entry->generation);
            TimeoutEntry* next = entry->next;
            FreeEntry(entry);
            entry = next;
        }

        current_ = (current_ + 1) % kWheelSize;
    }

private:
    TimingWheelBucket buckets_[kWheelSize];
    uint32_t current_ = 0;

    TimeoutEntry* entry_pool_;
    TimeoutEntry* free_list_;

    TimeoutEntry* AllocEntry() {
        if (!free_list_) return nullptr;
        TimeoutEntry* entry = free_list_;
        free_list_ = entry->next;
        return entry;
    }

    void FreeEntry(TimeoutEntry* entry) {
        entry->next = free_list_;
        free_list_ = entry;
    }
};

}  // namespace rdma
