// KV Protocol Definitions
// Protocol format for KV operations over RDMA

#pragma once

#include <cstdint>
#include <cstring>

namespace kv {

// KV operation types
enum class OpType : uint8_t {
    GET = 1,
    PUT = 2,
    GET_RESPONSE = 3,
    PUT_RESPONSE = 4,
};

// Response status codes
enum class Status : uint8_t {
    OK = 0,
    NOT_FOUND = 1,
    NO_SPACE = 2,
    INVALID_REQUEST = 3,
    INTERNAL_ERROR = 4,
};

// Request header (16 bytes, fixed size for easy parsing)
// For GET: only header is needed
// For PUT: header + segment descriptors (if multi-segment) + data
struct RequestHeader {
    uint8_t op_type;        // OpType
    uint8_t segment_count;  // Number of memory segments for PUT (0 means contiguous)
    uint16_t reserved;
    uint32_t value_size;    // Total value size (for PUT), 0 for GET
    uint64_t key;           // Key (uint64)

    void SetGet(uint64_t k) {
        op_type = static_cast<uint8_t>(OpType::GET);
        segment_count = 0;
        reserved = 0;
        value_size = 0;
        key = k;
    }

    void SetPut(uint64_t k, uint32_t size, uint8_t segments = 0) {
        op_type = static_cast<uint8_t>(OpType::PUT);
        segment_count = segments;
        reserved = 0;
        value_size = size;
        key = k;
    }
};

static_assert(sizeof(RequestHeader) == 16, "RequestHeader must be 16 bytes");

// Response header (16 bytes, fixed size)
struct ResponseHeader {
    uint8_t op_type;        // OpType (GET_RESPONSE or PUT_RESPONSE)
    uint8_t status;         // Status code
    uint16_t reserved;
    uint32_t value_size;    // Value size (for GET_RESPONSE)
    uint64_t key;           // Key (echo back for verification)

    void SetGetResponse(uint64_t k, Status s, uint32_t size = 0) {
        op_type = static_cast<uint8_t>(OpType::GET_RESPONSE);
        status = static_cast<uint8_t>(s);
        reserved = 0;
        value_size = size;
        key = k;
    }

    void SetPutResponse(uint64_t k, Status s) {
        op_type = static_cast<uint8_t>(OpType::PUT_RESPONSE);
        status = static_cast<uint8_t>(s);
        reserved = 0;
        value_size = 0;
        key = k;
    }
};

static_assert(sizeof(ResponseHeader) == 16, "ResponseHeader must be 16 bytes");

// Segment descriptor for non-contiguous memory PUT operations
// Describes one segment of the value data
struct SegmentDescriptor {
    uint32_t offset;    // Offset within the complete value
    uint32_t length;    // Length of this segment
};

static_assert(sizeof(SegmentDescriptor) == 8, "SegmentDescriptor must be 8 bytes");

// Constants
constexpr uint32_t MIN_VALUE_SIZE = 4096;           // 4KB minimum
constexpr uint32_t MAX_VALUE_SIZE = 65536;          // 64KB maximum
constexpr uint32_t VALUE_ALIGN = 4096;              // 4KB alignment
constexpr uint8_t MAX_SEGMENTS = 16;                // Max segments per PUT (matches max SGE)
constexpr size_t MAX_REQUEST_SIZE = sizeof(RequestHeader) +
                                    MAX_SEGMENTS * sizeof(SegmentDescriptor) +
                                    MAX_VALUE_SIZE;
constexpr size_t MAX_RESPONSE_SIZE = sizeof(ResponseHeader) + MAX_VALUE_SIZE;

// Helper functions
inline bool IsValidValueSize(uint32_t size) {
    return size >= MIN_VALUE_SIZE &&
           size <= MAX_VALUE_SIZE &&
           (size % VALUE_ALIGN) == 0;
}

inline uint32_t AlignValueSize(uint32_t size) {
    return ((size + VALUE_ALIGN - 1) / VALUE_ALIGN) * VALUE_ALIGN;
}

inline OpType GetOpType(const void* header) {
    return static_cast<OpType>(static_cast<const uint8_t*>(header)[0]);
}

}  // namespace kv
