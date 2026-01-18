#include "rdma/rpc/completion_dispatcher.h"

#include <cstring>

namespace rdma {

CompletionDispatcher::CompletionDispatcher() {
    // Initialize L0 cache (SIMD-friendly layout)
    for (size_t i = 0; i < kL0SetCount; ++i) {
        for (size_t w = 0; w < kL0Ways; ++w) {
            l0_cache_[i].conn_ids[w] = 0xFFFF;
            l0_cache_[i].generations[w] = 0;
            l0_cache_[i].access_counts[w] = 0;
            l0_cache_[i].ctxs[w] = nullptr;
            l0_cache_[i].handlers[w] = nullptr;
        }
    }

    // Initialize L1 hash hot cache
    std::memset(hot_cache_, 0xFF, sizeof(hot_cache_));
    for (size_t i = 0; i < kHotCacheSize; ++i) {
        hot_cache_[i].conn_id = 0xFFFF;
        hot_cache_[i].access_count = 0;
        hot_cache_[i].ctx = nullptr;
        hot_cache_[i].handler = nullptr;
    }

    // L2 two-level index (allocate on demand)
    std::memset(buckets_, 0, sizeof(buckets_));
}

CompletionDispatcher::~CompletionDispatcher() {
    // Free L2 buckets
    for (size_t i = 0; i < 256; ++i) {
        delete[] buckets_[i];
    }
}

void CompletionDispatcher::RegisterConnection(uint16_t conn_id, void* ctx, WcHandler handler) {
    // 1. Add to L2 two-level index (complete mapping)
    uint8_t bucket_idx = conn_id >> 8;
    uint8_t slot_idx = conn_id & 0xFF;

    if (!buckets_[bucket_idx]) {
        buckets_[bucket_idx] = new ConnectionEntry[256]();
    }
    buckets_[bucket_idx][slot_idx] = {ctx, handler};

    // 2. Update L0 cache (2-way set-associative, SIMD-friendly layout)
    size_t set_idx = conn_id & kL0SetMask;
    L0CacheSet& set = l0_cache_[set_idx];

    int target_way = -1;
    int lru_way = 0;
    uint8_t min_access = 255;

    for (size_t w = 0; w < kL0Ways; ++w) {
        if (set.conn_ids[w] == conn_id) {
            // Already exists, update directly
            target_way = static_cast<int>(w);
            break;
        }
        if (set.conn_ids[w] == 0xFFFF) {
            // Empty slot
            target_way = static_cast<int>(w);
            break;
        }
        // Record LRU candidate
        if (set.access_counts[w] < min_access) {
            min_access = set.access_counts[w];
            lru_way = static_cast<int>(w);
        }
    }

    // Use LRU replacement if no empty or existing slot
    if (target_way < 0) {
        target_way = lru_way;
    }

    set.conn_ids[target_way] = conn_id;
    set.generations[target_way]++;
    set.ctxs[target_way] = ctx;
    set.handlers[target_way] = handler;
    set.access_counts[target_way] = 128;  // Reset access count

    // 3. Update L1 hash hot cache
    InsertHotCache(conn_id, ctx, handler);
}

void CompletionDispatcher::UnregisterConnection(uint16_t conn_id) {
    // Remove from L2
    uint8_t bucket_idx = conn_id >> 8;
    uint8_t slot_idx = conn_id & 0xFF;

    if (buckets_[bucket_idx]) {
        buckets_[bucket_idx][slot_idx] = {nullptr, nullptr};
    }

    // Invalidate in L0
    size_t set_idx = conn_id & kL0SetMask;
    L0CacheSet& set = l0_cache_[set_idx];

    for (size_t w = 0; w < kL0Ways; ++w) {
        if (set.conn_ids[w] == conn_id) {
            set.conn_ids[w] = 0xFFFF;
            set.ctxs[w] = nullptr;
            set.handlers[w] = nullptr;
            break;
        }
    }

    // Invalidate in L1
    size_t idx = Hash(conn_id);
    for (int probe = 0; probe < 4; ++probe) {
        size_t pos = (idx + probe) & kHotCacheMask;
        if (hot_cache_[pos].conn_id == conn_id) {
            hot_cache_[pos].conn_id = 0xFFFF;
            hot_cache_[pos].ctx = nullptr;
            hot_cache_[pos].handler = nullptr;
            break;
        }
    }
}

void CompletionDispatcher::SetErrorHandler(void* ctx, ErrorHandler handler) {
    error_handler_.ctx = ctx;
    error_handler_.handler = handler;
}

void CompletionDispatcher::InsertHotCache(uint16_t conn_id, void* ctx, WcHandler handler) {
    size_t idx = Hash(conn_id);

    size_t best_pos = idx;
    uint16_t min_access = UINT16_MAX;

    for (int probe = 0; probe < 4; ++probe) {
        size_t pos = (idx + probe) & kHotCacheMask;

        // Already exists, update
        if (hot_cache_[pos].conn_id == conn_id) {
            hot_cache_[pos].ctx = ctx;
            hot_cache_[pos].handler = handler;
            hot_cache_[pos].access_count++;
            return;
        }

        // Empty slot, use directly
        if (hot_cache_[pos].conn_id == 0xFFFF) {
            best_pos = pos;
            break;
        }

        // Record LRU minimum
        if (hot_cache_[pos].access_count < min_access) {
            min_access = hot_cache_[pos].access_count;
            best_pos = pos;
        }
    }

    // Insert/replace
    hot_cache_[best_pos].conn_id = conn_id;
    hot_cache_[best_pos].ctx = ctx;
    hot_cache_[best_pos].handler = handler;
    hot_cache_[best_pos].access_count = 1;
}

void CompletionDispatcher::MaybePromoteToL0(uint16_t conn_id, void* ctx, WcHandler handler) {
    size_t set_idx = conn_id & kL0SetMask;
    L0CacheSet& set = l0_cache_[set_idx];

    int target_way = -1;

    for (size_t w = 0; w < kL0Ways; ++w) {
        if (set.conn_ids[w] == 0xFFFF) {
            // Empty slot
            target_way = static_cast<int>(w);
            break;
        }
        if (set.conn_ids[w] == conn_id) {
            // Already in L0
            return;
        }
    }

    // Choose lower access_count way for replacement
    if (target_way < 0) {
        target_way = (set.access_counts[0] <= set.access_counts[1]) ? 0 : 1;
    }

    set.conn_ids[target_way] = conn_id;
    set.generations[target_way]++;
    set.ctxs[target_way] = ctx;
    set.handlers[target_way] = handler;
    set.access_counts[target_way] = 128;  // Initial medium priority
}

void CompletionDispatcher::DispatchFromL2(uint16_t conn_id, const ibv_wc& wc) {
    uint8_t bucket_idx = conn_id >> 8;
    uint8_t slot_idx = conn_id & 0xFF;

    if (LIKELY(buckets_[bucket_idx])) {
        auto& entry = buckets_[bucket_idx][slot_idx];
        if (LIKELY(entry.handler)) {
            entry.handler(entry.ctx, wc);
            // Promote to L1 hash hot cache
            InsertHotCache(conn_id, entry.ctx, entry.handler);
        }
    }
}

void CompletionDispatcher::DispatchBatch(const ibv_wc* wc_array, int count,
        RpcSlotPool* slot_pool) {
    for (int i = 0; i < count; ++i) {
        // Stride prefetch: preload data for subsequent WCs
        if (i + 4 < count) {
            // Prefetch RpcSlotHot (16B), highest priority to L1
            uint32_t slot_idx = WrId::GetSlotIndex(wc_array[i + 4].wr_id);
            __builtin_prefetch(slot_pool->GetHot(slot_idx), 0, 3);
        }
        if (i + 8 < count) {
            // Prefetch Connection structure, lower priority to L2
            uint16_t conn_id = WrId::GetConnId(wc_array[i + 8].wr_id);
            __builtin_prefetch(GetConnectionFromL0(conn_id), 0, 1);
        }

        Dispatch(wc_array[i]);
    }
}

}  // namespace rdma
