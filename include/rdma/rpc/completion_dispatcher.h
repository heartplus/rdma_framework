#pragma once

#include <infiniband/verbs.h>

#include <cstdint>
#include <cstring>

#include "rdma/common/types.h"
#include "rdma/common/wr_id.h"
#include "rdma/rpc/rpc_slot.h"

namespace rdma {

// Completion Dispatcher with 3-level cache
// Design considerations:
// 1. Direct 65536 array causes severe cache misses
// 2. Three-level cache structure:
//    - L0: 2-way set-associative cache (32 sets), fastest
//    - L1: Open-address hash table (64 entries), handles L0 misses
//    - L2: Two-level index (256 buckets × 256 connections), complete mapping
// 3. At 1M QPS, usually only a few connections contribute most traffic
//    L0 cache hit rate is very high
class CompletionDispatcher {
public:
    using WcHandler = void (*)(void* ctx, const ibv_wc& wc);
    using ErrorHandler = void (*)(void* ctx, uint16_t conn_id, ibv_wc_status status);

    // L0 cache configuration (2-way set-associative)
    static constexpr size_t kL0SetCount = 32;
    static constexpr size_t kL0SetMask = kL0SetCount - 1;
    static constexpr size_t kL0Ways = 2;

    // L0 cache entry
    struct L0CacheEntry {
        uint16_t conn_id;
        uint8_t generation;
        uint8_t access_count;
        void* ctx;
        WcHandler handler;
    };

    // L0 cache set (SIMD-friendly layout)
    struct alignas(64) L0CacheSet {
        uint16_t conn_ids[kL0Ways];
        uint8_t access_counts[kL0Ways];
        uint8_t generations[kL0Ways];
        void* ctxs[kL0Ways];
        WcHandler handlers[kL0Ways];

        L0CacheEntry GetEntry(size_t way) const {
            return {conn_ids[way], generations[way], access_counts[way], ctxs[way], handlers[way]};
        }
    };

    CompletionDispatcher();
    ~CompletionDispatcher();

    // Register connection handler
    void RegisterConnection(uint16_t conn_id, void* ctx, WcHandler handler);

    // Unregister connection
    void UnregisterConnection(uint16_t conn_id);

    // Set error handler
    void SetErrorHandler(void* ctx, ErrorHandler handler);

    // Hot path: dispatch completion (fully inline, 3-level cache lookup)
    HOT_FUNCTION ALWAYS_INLINE void Dispatch(const ibv_wc& wc) {
        // 1. Fast failure path
        if (UNLIKELY(wc.status != IBV_WC_SUCCESS)) {
            uint16_t conn_id = WrId::GetConnId(wc.wr_id);
            if (error_handler_.handler) {
                error_handler_.handler(error_handler_.ctx, conn_id, wc.status);
            }
            return;
        }

        uint16_t conn_id = WrId::GetConnId(wc.wr_id);

        // 2. L0 cache lookup (2-way set-associative)
        size_t set_idx = conn_id & kL0SetMask;
        L0CacheSet& set = l0_cache_[set_idx];

        // Try way 0
        if (LIKELY(set.conn_ids[0] == conn_id)) {
            set.access_counts[0] = 255;
            set.handlers[0](set.ctxs[0], wc);
            if (set.access_counts[1] > 0) --set.access_counts[1];
            return;
        }

        // Try way 1
        if (LIKELY(set.conn_ids[1] == conn_id)) {
            set.access_counts[1] = 255;
            set.handlers[1](set.ctxs[1], wc);
            if (set.access_counts[0] > 0) --set.access_counts[0];
            return;
        }

        // 3. L1 hash hot cache lookup
        auto* entry = FindInHotCache(conn_id);
        if (LIKELY(entry)) {
            entry->handler(entry->ctx, wc);
            entry->access_count++;
            MaybePromoteToL0(conn_id, entry->ctx, entry->handler);
            return;
        }

        // 4. L2 two-level index lookup (cold path)
        DispatchFromL2(conn_id, wc);
    }

    // Batch dispatch with software prefetch
    HOT_FUNCTION void DispatchBatch(const ibv_wc* wc_array, int count, RpcSlotPool* slot_pool);

    // Simple batch dispatch (for small counts)
    HOT_FUNCTION ALWAYS_INLINE void DispatchBatchSimple(const ibv_wc* wc_array, int count) {
        for (int i = 0; i < count; ++i) {
            Dispatch(wc_array[i]);
        }
    }

    // Get connection pointer from L0 cache (for prefetch)
    ALWAYS_INLINE void* GetConnectionFromL0(uint16_t conn_id) {
        size_t set_idx = conn_id & kL0SetMask;
        return l0_cache_[set_idx].ctxs[0];
    }

private:
    // L1 hash cache configuration
    static constexpr size_t kHotCacheSize = 64;
    static constexpr size_t kHotCacheMask = kHotCacheSize - 1;

    struct ConnectionEntry {
        void* ctx = nullptr;
        WcHandler handler = nullptr;
    };

    struct HotCacheEntry {
        uint16_t conn_id = 0xFFFF;
        uint16_t access_count = 0;
        void* ctx = nullptr;
        WcHandler handler = nullptr;
    };

    // L0 2-way set-associative cache (fastest)
    alignas(64) L0CacheSet l0_cache_[kL0SetCount];

    // L1 hash hot cache
    alignas(64) HotCacheEntry hot_cache_[kHotCacheSize];

    // L2 two-level index
    ConnectionEntry* buckets_[256];

    struct {
        void* ctx = nullptr;
        ErrorHandler handler = nullptr;
    } error_handler_;

    // Hash function (simple but efficient)
    ALWAYS_INLINE size_t Hash(uint16_t conn_id) const {
        return (conn_id * 0x9E3779B9u) & kHotCacheMask;
    }

    // O(1) hot cache lookup
    ALWAYS_INLINE HotCacheEntry* FindInHotCache(uint16_t conn_id) {
        size_t idx = Hash(conn_id);

        for (int probe = 0; probe < 4; ++probe) {
            size_t pos = (idx + probe) & kHotCacheMask;
            if (hot_cache_[pos].conn_id == conn_id) {
                return &hot_cache_[pos];
            }
            if (hot_cache_[pos].conn_id == 0xFFFF) {
                return nullptr;
            }
        }
        return nullptr;
    }

    void InsertHotCache(uint16_t conn_id, void* ctx, WcHandler handler);
    void MaybePromoteToL0(uint16_t conn_id, void* ctx, WcHandler handler);
    void DispatchFromL2(uint16_t conn_id, const ibv_wc& wc);
};

}  // namespace rdma
