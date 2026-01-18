#include "rdma/rpc/rpc_slot.h"

#include <cstring>

namespace rdma {

RpcSlotPool::RpcSlotPool() {
    // Pre-allocate Hot data (16 bytes × N, 4 slots share one cache line)
    slots_hot_ = static_cast<RpcSlotHot*>(aligned_alloc(64, kMaxSlots * sizeof(RpcSlotHot)));

    // Pre-allocate Context data (32 bytes × N, accessed only on completion)
    slots_context_ =
            static_cast<RpcSlotContext*>(aligned_alloc(64, kMaxSlots * sizeof(RpcSlotContext)));

    // Pre-allocate Extended data (64 bytes × N, cold data)
    extended_info_ =
            static_cast<ExtendedSlotInfo*>(aligned_alloc(64, kMaxSlots * sizeof(ExtendedSlotInfo)));

    // Pre-allocate fixed-size free list array
    free_stack_ = static_cast<uint32_t*>(aligned_alloc(64, kMaxSlots * sizeof(uint32_t)));

    // Initialize all memory to zero
    memset(slots_hot_, 0, kMaxSlots * sizeof(RpcSlotHot));
    memset(slots_context_, 0, kMaxSlots * sizeof(RpcSlotContext));
    memset(extended_info_, 0, kMaxSlots * sizeof(ExtendedSlotInfo));

    // Pre-fill free stack (reverse fill so low indices are allocated first)
    for (uint32_t i = 0; i < kMaxSlots; ++i) {
        // Use slot_gen_packed to store: low 20 bits = slot_index, high 12 bits = generation(0)
        slots_hot_[i].slot_gen_packed = i;  // generation = 0, slot_index = i
        slots_hot_[i].flags = 0;            // state=free, wr_type=0, signaled=false
        slots_context_[i].extended_info_index = i;  // 1:1 mapping
        free_stack_[i] = kMaxSlots - 1 - i;         // Reverse
    }
    stack_top_ = kMaxSlots;  // Stack full
}

RpcSlotPool::~RpcSlotPool() {
    free(slots_hot_);
    free(slots_context_);
    free(extended_info_);
    free(free_stack_);
}

}  // namespace rdma
