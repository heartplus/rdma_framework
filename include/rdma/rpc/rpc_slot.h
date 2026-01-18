#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include "rdma/common/types.h"

namespace rdma {

// RpcSlotHot: Hot data accessed in every PollCQ iteration (16 bytes)
// Design: Pack into 16 bytes for 4 slots per cache line
struct alignas(16) RpcSlotHot {
    // Combined field: slot_index(20) + generation(12) = 32 bits
    uint32_t slot_gen_packed;

    // Flags field layout: [7:6]=state, [5:3]=wr_type, [2]=signaled, [1:0]=reserved
    uint8_t flags;
    uint8_t _padding1;

    uint16_t conn_id;

    uint8_t total_wr_count;
    uint8_t completed_wr_count;
    uint8_t error_code;
    uint8_t _padding2;

    uint32_t timeout_tick;

    // Flag bit definitions
    static constexpr uint8_t kStateMask = 0xC0;   // bits [7:6]
    static constexpr uint8_t kStateShift = 6;
    static constexpr uint8_t kWrTypeMask = 0x38;  // bits [5:3]
    static constexpr uint8_t kWrTypeShift = 3;
    static constexpr uint8_t kSignaledBit = 0x04; // bit [2]

    // State enum values
    static constexpr uint8_t kStateFree = 0;
    static constexpr uint8_t kStateInUse = 1;
    static constexpr uint8_t kStateCompleting = 2;

    // Slot index and generation masks
    static constexpr uint32_t kSlotIndexMask = 0x000FFFFF;  // Low 20 bits
    static constexpr uint32_t kGenerationShift = 20;
    static constexpr uint32_t kGenerationMask = 0xFFF;       // High 12 bits (4096)
    static constexpr uint32_t kMaxSlotIndex = (1u << 20) - 1;

    // Flags accessors
    ALWAYS_INLINE uint8_t GetState() const { return (flags & kStateMask) >> kStateShift; }

    ALWAYS_INLINE void SetState(uint8_t state) {
        flags = (flags & ~kStateMask) | ((state << kStateShift) & kStateMask);
    }

    ALWAYS_INLINE uint8_t GetWrType() const { return (flags & kWrTypeMask) >> kWrTypeShift; }

    ALWAYS_INLINE void SetWrType(uint8_t wr_type) {
        flags = (flags & ~kWrTypeMask) | ((wr_type << kWrTypeShift) & kWrTypeMask);
    }

    ALWAYS_INLINE bool IsSignaled() const { return flags & kSignaledBit; }

    ALWAYS_INLINE void SetSignaled(bool signaled) {
        if (signaled) {
            flags |= kSignaledBit;
        } else {
            flags &= ~kSignaledBit;
        }
    }

    // Slot index and generation accessors
    ALWAYS_INLINE uint32_t GetSlotIndex() const { return slot_gen_packed & kSlotIndexMask; }

    ALWAYS_INLINE uint16_t GetGeneration() const {
        return static_cast<uint16_t>((slot_gen_packed >> kGenerationShift) & kGenerationMask);
    }

    ALWAYS_INLINE void SetSlotIndex(uint32_t idx) {
        slot_gen_packed = (slot_gen_packed & ~kSlotIndexMask) | (idx & kSlotIndexMask);
    }

    ALWAYS_INLINE void IncrementGeneration() {
        uint32_t gen = (slot_gen_packed >> kGenerationShift) + 1;
        slot_gen_packed = (slot_gen_packed & kSlotIndexMask) |
                          ((gen & kGenerationMask) << kGenerationShift);
    }

    ALWAYS_INLINE uint32_t GetSlotGenPacked() const { return slot_gen_packed; }

    // Hot path methods
    ALWAYS_INLINE bool IsCompleted() const { return completed_wr_count >= total_wr_count; }

    ALWAYS_INLINE bool IsFree() const { return GetState() == kStateFree; }

    ALWAYS_INLINE void MarkCompleted() { ++completed_wr_count; }

    ALWAYS_INLINE bool ValidateGeneration(uint16_t expected_gen) const {
        return GetGeneration() == expected_gen;
    }
};

static_assert(sizeof(RpcSlotHot) == 16, "RpcSlotHot must be 16 bytes");

// RpcSlotContext: Context data accessed only on RPC completion (32 bytes)
struct alignas(32) RpcSlotContext {
    // Callback info
    void (*callback_fn)(void* ctx, uint32_t slot_index, uint8_t status);
    void* callback_ctx;

    // Buffer info
    void* recv_buffer;
    uint32_t recv_length;
    uint32_t extended_info_index;
};

static_assert(sizeof(RpcSlotContext) == 32, "RpcSlotContext must be 32 bytes");

// ExtendedSlotInfo: Cold data for debugging/statistics (64 bytes)
struct alignas(64) ExtendedSlotInfo {
    uint64_t rpc_id;
    uint64_t start_tsc;
    void* user_data;
    uint32_t request_size;
    uint32_t response_size;

    struct WrDetail {
        uint8_t posted : 1;
        uint8_t completed : 1;
        uint8_t status : 6;
    };
    WrDetail wr_details[4];

    uint8_t _padding[24];
};

static_assert(sizeof(ExtendedSlotInfo) == 64, "ExtendedSlotInfo must be 64 bytes");

// RpcSlot: Logical combination of Hot + Context
struct RpcSlot {
    RpcSlotHot* hot;
    RpcSlotContext* context;

    ALWAYS_INLINE bool IsCompleted() const { return hot->IsCompleted(); }
    ALWAYS_INLINE bool IsFree() const { return hot->IsFree(); }
    ALWAYS_INLINE void MarkCompleted() { hot->MarkCompleted(); }
};

// RpcSlot Pool: Heat-layered + fixed-size stack
class RpcSlotPool {
public:
    static constexpr size_t kMaxSlots = 65536;

    RpcSlotPool();
    ~RpcSlotPool();

    // Non-copyable
    RpcSlotPool(const RpcSlotPool&) = delete;
    RpcSlotPool& operator=(const RpcSlotPool&) = delete;

    // Allocate slot (fixed-size stack, no dynamic allocation)
    ALWAYS_INLINE std::pair<uint32_t, uint8_t> Allocate() {
        if (UNLIKELY(stack_top_ == 0)) {
            return {UINT32_MAX, 0};
        }
        uint32_t index = free_stack_[--stack_top_];

        RpcSlotHot* hot = &slots_hot_[index];
        hot->SetState(RpcSlotHot::kStateInUse);
        hot->completed_wr_count = 0;
        hot->error_code = 0;

        return {index, static_cast<uint8_t>(hot->GetGeneration())};
    }

    // Free slot (direct push to stack)
    ALWAYS_INLINE void Free(uint32_t slot_index) {
        RpcSlotHot* hot = &slots_hot_[slot_index];
        hot->SetState(RpcSlotHot::kStateFree);
        hot->IncrementGeneration();
        free_stack_[stack_top_++] = slot_index;
    }

    // Get Hot data (PollCQ hot path)
    ALWAYS_INLINE RpcSlotHot* GetHot(uint32_t slot_index) { return &slots_hot_[slot_index]; }

    // Get Hot data with generation check
    ALWAYS_INLINE RpcSlotHot* GetHotChecked(uint32_t slot_index, uint8_t expected_gen) {
        RpcSlotHot* hot = &slots_hot_[slot_index];
        if (UNLIKELY(!hot->ValidateGeneration(expected_gen))) {
            return nullptr;
        }
        return hot;
    }

    // Get Context data (only accessed on completion)
    ALWAYS_INLINE RpcSlotContext* GetContext(uint32_t slot_index) {
        return &slots_context_[slot_index];
    }

    // Get Extended info (cold path)
    ALWAYS_INLINE ExtendedSlotInfo* GetExtendedInfo(uint32_t slot_index) {
        return &extended_info_[slot_index];
    }

    // Prefetch Hot data to cache (PollCQ prefetch)
    ALWAYS_INLINE void PrefetchHot(uint32_t slot_index) {
        __builtin_prefetch(&slots_hot_[slot_index], 0, 3);
    }

    // Prefetch Context data (only when callback is needed)
    ALWAYS_INLINE void PrefetchContext(uint32_t slot_index) {
        __builtin_prefetch(&slots_context_[slot_index], 0, 3);
    }

    // Get available slot count
    ALWAYS_INLINE size_t AvailableCount() const { return stack_top_; }

private:
    RpcSlotHot* slots_hot_;
    RpcSlotContext* slots_context_;
    ExtendedSlotInfo* extended_info_;
    uint32_t* free_stack_;
    size_t stack_top_ = 0;
};

}  // namespace rdma
