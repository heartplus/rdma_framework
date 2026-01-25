// KV Store Implementation
// Pre-allocated hash map with fixed-size storage pool
// Designed to avoid dynamic memory allocation after initialization

#pragma once

#include <sys/uio.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "kv_protocol.h"

namespace kv {

// Configuration for KV Store
struct StoreConfig {
    size_t total_storage_size = 1ULL * 1024 * 1024 * 1024;  // 1GB default
    size_t slot_size = MAX_VALUE_SIZE;                       // 64KB per slot
    size_t hash_table_size = 0;                              // 0 = auto-calculate
};

// KV Store with pre-allocated storage
// Design:
// 1. Pre-allocate a large memory pool for value storage (divided into fixed-size slots)
// 2. Use open-addressing hash table with linear probing (no dynamic allocation)
// 3. Value storage pool does NOT need RDMA registration (server-side only)
// 4. Memory is copied from/to RDMA buffers during send/recv
class KVStore {
public:
    // Hash table entry
    struct Entry {
        uint64_t key;           // Key (0 = empty slot)
        uint32_t slot_index;    // Index in value storage pool
        uint32_t value_size;    // Actual value size
        bool occupied;          // Is this entry occupied?
        bool deleted;           // Tombstone for deletion
    };

    KVStore() = default;
    ~KVStore() = default;

    // Initialize the store
    // Returns false if allocation fails
    bool Initialize(const StoreConfig& config = StoreConfig()) {
        config_ = config;

        // Calculate number of slots
        num_slots_ = config_.total_storage_size / config_.slot_size;
        if (num_slots_ == 0) {
            return false;
        }

        // Calculate hash table size (use 2x slots for good load factor)
        if (config_.hash_table_size == 0) {
            hash_table_size_ = NextPowerOfTwo(num_slots_ * 2);
        } else {
            hash_table_size_ = NextPowerOfTwo(config_.hash_table_size);
        }
        hash_mask_ = hash_table_size_ - 1;

        // Allocate value storage pool
        storage_pool_.reset(new (std::nothrow) uint8_t[config_.total_storage_size]);
        if (!storage_pool_) {
            return false;
        }

        // Allocate hash table
        hash_table_.resize(hash_table_size_);
        for (auto& entry : hash_table_) {
            entry.key = 0;
            entry.slot_index = 0;
            entry.value_size = 0;
            entry.occupied = false;
            entry.deleted = false;
        }

        // Initialize free slot list (simple stack-based free list)
        free_slots_.reserve(num_slots_);
        for (size_t i = 0; i < num_slots_; ++i) {
            free_slots_.push_back(static_cast<uint32_t>(num_slots_ - 1 - i));
        }

        used_slots_ = 0;
        used_bytes_ = 0;

        return true;
    }

    // PUT operation
    // Returns Status::OK on success
    // value_data points to the value bytes, value_size is the total size
    Status Put(uint64_t key, const void* value_data, uint32_t value_size) {
        if (!IsValidValueSize(value_size)) {
            return Status::INVALID_REQUEST;
        }

        // Check if we have space
        if (free_slots_.empty()) {
            return Status::NO_SPACE;
        }

        // Check if key already exists (update case)
        Entry* existing = FindEntry(key);
        if (existing && existing->occupied && !existing->deleted) {
            // Update existing entry
            uint8_t* slot_ptr = GetSlotPtr(existing->slot_index);
            std::memcpy(slot_ptr, value_data, value_size);
            used_bytes_ -= existing->value_size;
            existing->value_size = value_size;
            used_bytes_ += value_size;
            return Status::OK;
        }

        // Find a slot for the new entry
        size_t hash = HashKey(key);
        size_t index = hash & hash_mask_;

        // Linear probing to find empty or deleted slot
        for (size_t i = 0; i < hash_table_size_; ++i) {
            Entry& entry = hash_table_[index];
            if (!entry.occupied || entry.deleted) {
                // Allocate storage slot
                uint32_t slot_index = free_slots_.back();
                free_slots_.pop_back();

                // Copy value data
                uint8_t* slot_ptr = GetSlotPtr(slot_index);
                std::memcpy(slot_ptr, value_data, value_size);

                // Update hash table entry
                entry.key = key;
                entry.slot_index = slot_index;
                entry.value_size = value_size;
                entry.occupied = true;
                entry.deleted = false;

                ++used_slots_;
                used_bytes_ += value_size;

                return Status::OK;
            }
            index = (index + 1) & hash_mask_;
        }

        // Should not reach here if hash table is properly sized
        return Status::NO_SPACE;
    }

    // PUT operation with scatter-gather (non-contiguous memory)
    // segments array contains pointers and lengths of each segment
    Status PutSGL(uint64_t key, const struct iovec* segments, size_t segment_count,
                  uint32_t total_size) {
        if (!IsValidValueSize(total_size) || segment_count == 0) {
            return Status::INVALID_REQUEST;
        }

        // Verify total size matches
        uint32_t calculated_size = 0;
        for (size_t i = 0; i < segment_count; ++i) {
            calculated_size += segments[i].iov_len;
        }
        if (calculated_size != total_size) {
            return Status::INVALID_REQUEST;
        }

        // Check if we have space
        if (free_slots_.empty()) {
            return Status::NO_SPACE;
        }

        // Check if key already exists (update case)
        Entry* existing = FindEntry(key);
        uint32_t slot_index;

        if (existing && existing->occupied && !existing->deleted) {
            slot_index = existing->slot_index;
            used_bytes_ -= existing->value_size;
            existing->value_size = total_size;
        } else {
            // Find new slot
            size_t hash = HashKey(key);
            size_t index = hash & hash_mask_;

            for (size_t i = 0; i < hash_table_size_; ++i) {
                Entry& entry = hash_table_[index];
                if (!entry.occupied || entry.deleted) {
                    slot_index = free_slots_.back();
                    free_slots_.pop_back();

                    entry.key = key;
                    entry.slot_index = slot_index;
                    entry.value_size = total_size;
                    entry.occupied = true;
                    entry.deleted = false;
                    ++used_slots_;
                    break;
                }
                index = (index + 1) & hash_mask_;
                if (i == hash_table_size_ - 1) {
                    return Status::NO_SPACE;
                }
            }
        }

        // Copy segments to slot
        uint8_t* slot_ptr = GetSlotPtr(slot_index);
        size_t offset = 0;
        for (size_t i = 0; i < segment_count; ++i) {
            std::memcpy(slot_ptr + offset, segments[i].iov_base, segments[i].iov_len);
            offset += segments[i].iov_len;
        }

        used_bytes_ += total_size;
        return Status::OK;
    }

    // GET operation
    // Returns Status::OK on success, copies value to out_value
    // out_value_size returns the actual value size
    Status Get(uint64_t key, void* out_value, uint32_t buffer_size,
               uint32_t* out_value_size) {
        Entry* entry = FindEntry(key);
        if (!entry || !entry->occupied || entry->deleted) {
            return Status::NOT_FOUND;
        }

        if (buffer_size < entry->value_size) {
            return Status::INVALID_REQUEST;
        }

        uint8_t* slot_ptr = GetSlotPtr(entry->slot_index);
        std::memcpy(out_value, slot_ptr, entry->value_size);
        *out_value_size = entry->value_size;

        return Status::OK;
    }

    // GET operation - just get the value info without copying
    Status GetInfo(uint64_t key, uint32_t* out_value_size) {
        Entry* entry = FindEntry(key);
        if (!entry || !entry->occupied || entry->deleted) {
            return Status::NOT_FOUND;
        }
        *out_value_size = entry->value_size;
        return Status::OK;
    }

    // Get pointer to value data (zero-copy read)
    // WARNING: The returned pointer is only valid until the next PUT/DELETE
    const void* GetValuePtr(uint64_t key, uint32_t* out_value_size) {
        Entry* entry = FindEntry(key);
        if (!entry || !entry->occupied || entry->deleted) {
            return nullptr;
        }
        *out_value_size = entry->value_size;
        return GetSlotPtr(entry->slot_index);
    }

    // Statistics
    size_t GetUsedSlots() const { return used_slots_; }
    size_t GetTotalSlots() const { return num_slots_; }
    size_t GetUsedBytes() const { return used_bytes_; }
    size_t GetTotalBytes() const { return config_.total_storage_size; }
    float GetLoadFactor() const {
        return num_slots_ > 0 ? static_cast<float>(used_slots_) / num_slots_ : 0.0f;
    }

private:
    // Hash function (FNV-1a variant for uint64)
    static size_t HashKey(uint64_t key) {
        const uint64_t FNV_PRIME = 0x100000001b3;
        const uint64_t FNV_OFFSET = 0xcbf29ce484222325;

        uint64_t hash = FNV_OFFSET;
        for (int i = 0; i < 8; ++i) {
            hash ^= (key & 0xFF);
            hash *= FNV_PRIME;
            key >>= 8;
        }
        return static_cast<size_t>(hash);
    }

    // Find entry by key
    Entry* FindEntry(uint64_t key) {
        size_t hash = HashKey(key);
        size_t index = hash & hash_mask_;

        for (size_t i = 0; i < hash_table_size_; ++i) {
            Entry& entry = hash_table_[index];
            if (!entry.occupied) {
                return nullptr;  // Empty slot, key not found
            }
            if (entry.key == key && !entry.deleted) {
                return &entry;
            }
            index = (index + 1) & hash_mask_;
        }
        return nullptr;
    }

    // Get pointer to storage slot
    uint8_t* GetSlotPtr(uint32_t slot_index) {
        return storage_pool_.get() + (static_cast<size_t>(slot_index) * config_.slot_size);
    }

    // Next power of two
    static size_t NextPowerOfTwo(size_t n) {
        if (n == 0) return 1;
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }

    StoreConfig config_;
    std::unique_ptr<uint8_t[]> storage_pool_;
    std::vector<Entry> hash_table_;
    std::vector<uint32_t> free_slots_;

    size_t num_slots_ = 0;
    size_t hash_table_size_ = 0;
    size_t hash_mask_ = 0;
    size_t used_slots_ = 0;
    size_t used_bytes_ = 0;
};

}  // namespace kv
