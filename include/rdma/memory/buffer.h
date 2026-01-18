#pragma once

#include <infiniband/verbs.h>
#include <sys/uio.h>

#include <cstddef>
#include <cstdint>

#include "rdma/common/types.h"

namespace rdma {

// Buffer abstraction for RDMA operations
// Design considerations:
// 1. Zero-copy: references registered memory
// 2. Supports SGL (Scatter-Gather List)
// 3. Must be from pre-registered memory, not stack!
class Buffer {
public:
    Buffer() : addr_(nullptr), length_(0), lkey_(0) {}

    Buffer(void* addr, size_t length, uint32_t lkey)
            : addr_(addr), length_(length), lkey_(lkey) {}

    // Check if buffer is valid
    bool IsValid() const { return addr_ != nullptr && length_ > 0; }

    // Accessors
    void* GetAddr() const { return addr_; }
    uint8_t* GetData() const { return static_cast<uint8_t*>(addr_); }
    size_t GetLength() const { return length_; }
    uint32_t GetLkey() const { return lkey_; }

    // Get as iovec
    struct iovec ToIovec() const {
        return {addr_, length_};
    }

    // Get sub-buffer (slice)
    Buffer Slice(size_t offset, size_t length) const {
        if (offset >= length_ || offset + length > length_) {
            return Buffer();
        }
        return Buffer(static_cast<uint8_t*>(addr_) + offset, length, lkey_);
    }

    // Get remote memory info (for RDMA write/read targets)
    RemoteMemoryInfo ToRemoteInfo(uint32_t rkey) const {
        return {reinterpret_cast<uint64_t>(addr_), rkey, static_cast<uint32_t>(length_)};
    }

private:
    void* addr_;
    size_t length_;
    uint32_t lkey_;
};

// SGE (Scatter-Gather Element) filler for performance optimization
// Design: Use template unrolling to avoid loop overhead
class SgeFiller {
public:
    static constexpr size_t MAX_SGE = 16;

    // Compile-time unrolled SGE fill
    template <size_t N>
    ALWAYS_INLINE static void FillUnrolled(ibv_sge* sge_array, const struct iovec* iov,
            uint32_t lkey) {
        if constexpr (N >= 1) {
            sge_array[0].addr = reinterpret_cast<uint64_t>(iov[0].iov_base);
            sge_array[0].length = static_cast<uint32_t>(iov[0].iov_len);
            sge_array[0].lkey = lkey;
        }
        if constexpr (N >= 2) {
            sge_array[1].addr = reinterpret_cast<uint64_t>(iov[1].iov_base);
            sge_array[1].length = static_cast<uint32_t>(iov[1].iov_len);
            sge_array[1].lkey = lkey;
        }
        if constexpr (N > 2) {
            FillUnrolled<N - 2>(sge_array + 2, iov + 2, lkey);
        }
    }

    // Runtime dispatch to compile-time unrolled version
    HOT_FUNCTION ALWAYS_INLINE static void Fill(ibv_sge* sge_array, const struct iovec* iov,
            size_t iov_count, uint32_t lkey) {
        switch (iov_count) {
            case 1:
                FillUnrolled<1>(sge_array, iov, lkey);
                return;
            case 2:
                FillUnrolled<2>(sge_array, iov, lkey);
                return;
            case 3:
                FillUnrolled<3>(sge_array, iov, lkey);
                return;
            case 4:
                FillUnrolled<4>(sge_array, iov, lkey);
                return;
            case 5:
                FillUnrolled<5>(sge_array, iov, lkey);
                return;
            case 6:
                FillUnrolled<6>(sge_array, iov, lkey);
                return;
            case 7:
                FillUnrolled<7>(sge_array, iov, lkey);
                return;
            case 8:
                FillUnrolled<8>(sge_array, iov, lkey);
                return;
            default:
                // Fallback to loop for large counts
                for (size_t i = 0; i < iov_count; ++i) {
                    sge_array[i].addr = reinterpret_cast<uint64_t>(iov[i].iov_base);
                    sge_array[i].length = static_cast<uint32_t>(iov[i].iov_len);
                    sge_array[i].lkey = lkey;
                }
        }
    }
};

// SGE alignment checker for PCIe optimization
class SgeAlignmentChecker {
public:
    struct AlignmentReport {
        size_t total_sge;
        size_t page_aligned_count;
        size_t cache_aligned_count;
        size_t unaligned_count;
        size_t total_unaligned_bytes;

        float AlignmentRatio() const {
            return total_sge > 0 ? static_cast<float>(page_aligned_count) / total_sge : 1.0f;
        }

        bool IsOptimal() const { return unaligned_count == 0; }
    };

    ALWAYS_INLINE static AlignmentReport Check(const struct iovec* iov, size_t iov_count) {
        AlignmentReport report = {};
        report.total_sge = iov_count;

        for (size_t i = 0; i < iov_count; ++i) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(iov[i].iov_base);

            if ((addr & (kPageSize - 1)) == 0) {
                ++report.page_aligned_count;
                ++report.cache_aligned_count;
            } else if ((addr & (kCacheLineSize - 1)) == 0) {
                ++report.cache_aligned_count;
                ++report.unaligned_count;
            } else {
                ++report.unaligned_count;
                report.total_unaligned_bytes += iov[i].iov_len;
            }
        }

        return report;
    }

    ALWAYS_INLINE static bool AllPageAligned(const struct iovec* iov, size_t iov_count) {
        for (size_t i = 0; i < iov_count; ++i) {
            if (reinterpret_cast<uintptr_t>(iov[i].iov_base) & (kPageSize - 1)) {
                return false;
            }
        }
        return true;
    }
};

}  // namespace rdma
