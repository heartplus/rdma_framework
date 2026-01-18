#pragma once

#include <memory>
#include <string>

#include "rdma/common/status.h"
#include "rdma/common/types.h"

namespace rdma {

// Handshake message for QP connection establishment
struct HandshakeMessage {
    QpConnParams qp_params;
    RemoteMemoryInfo recv_buffer_info;
    uint16_t conn_id;
    uint16_t version;
    uint32_t flags;
    char hostname[64];
};

// Abstract handshake channel interface
class HandshakeChannel {
public:
    virtual ~HandshakeChannel() = default;

    // Send handshake message
    virtual Status Send(const HandshakeMessage& msg) = 0;

    // Receive handshake message
    virtual Status Receive(HandshakeMessage& msg) = 0;

    // Close channel
    virtual void Close() = 0;

    // Check if channel is connected
    virtual bool IsConnected() const = 0;

    // Get peer address
    virtual std::string GetPeerAddress() const = 0;
};

}  // namespace rdma
