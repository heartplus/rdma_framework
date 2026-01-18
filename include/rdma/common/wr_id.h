#pragma once

#include <cstdint>

#include "rdma/common/types.h"

namespace rdma {

// wr_id 64-bit layout:
// [ 16 bits conn_id | 8 bits opcode | 8 bits generation | 32 bits slot_index ]
//
// Design considerations:
// - generation prevents ABA problem when slot is reused
// - completion validates generation, mismatches are discarded

class WrId {
public:
    // Encode wr_id (all inline to avoid function call overhead)
    ALWAYS_INLINE static uint64_t Encode(uint16_t conn_id, RpcOpcode opcode, uint8_t generation,
            uint32_t slot_index) {
        return (static_cast<uint64_t>(conn_id) << 48) |
               (static_cast<uint64_t>(static_cast<uint8_t>(opcode)) << 40) |
               (static_cast<uint64_t>(generation) << 32) | slot_index;
    }

    ALWAYS_INLINE static uint16_t GetConnId(uint64_t wr_id) {
        return static_cast<uint16_t>(wr_id >> 48);
    }

    ALWAYS_INLINE static RpcOpcode GetOpcode(uint64_t wr_id) {
        return static_cast<RpcOpcode>((wr_id >> 40) & 0xFF);
    }

    ALWAYS_INLINE static uint8_t GetGeneration(uint64_t wr_id) {
        return static_cast<uint8_t>((wr_id >> 32) & 0xFF);
    }

    ALWAYS_INLINE static uint32_t GetSlotIndex(uint64_t wr_id) {
        return static_cast<uint32_t>(wr_id & 0xFFFFFFFF);
    }
};

// ImmData codec for bidirectional zero-copy optimization
// ImmData layout (32-bit):
// [ 16 bits conn_id | 8 bits opcode | 8 bits slot_index_low ]
namespace ImmDataCodec {

ALWAYS_INLINE uint32_t Encode(uint16_t conn_id, uint8_t opcode, uint8_t slot_hint) {
    return (static_cast<uint32_t>(conn_id) << 16) | (static_cast<uint32_t>(opcode) << 8) |
           slot_hint;
}

ALWAYS_INLINE uint16_t GetConnId(uint32_t imm_data) {
    return static_cast<uint16_t>(imm_data >> 16);
}

ALWAYS_INLINE uint8_t GetOpcode(uint32_t imm_data) {
    return static_cast<uint8_t>((imm_data >> 8) & 0xFF);
}

ALWAYS_INLINE uint8_t GetSlotHint(uint32_t imm_data) {
    return static_cast<uint8_t>(imm_data & 0xFF);
}

}  // namespace ImmDataCodec

}  // namespace rdma
