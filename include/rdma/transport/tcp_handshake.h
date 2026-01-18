#pragma once

#include <memory>
#include <string>

#include "rdma/common/status.h"
#include "rdma/transport/handshake_channel.h"

namespace rdma {

// TCP-based handshake channel for QP connection establishment
class TcpHandshakeChannel : public HandshakeChannel {
public:
    ~TcpHandshakeChannel() override;

    // Non-copyable
    TcpHandshakeChannel(const TcpHandshakeChannel&) = delete;
    TcpHandshakeChannel& operator=(const TcpHandshakeChannel&) = delete;

    // Factory methods
    // Client: connect to server
    static Result<std::unique_ptr<TcpHandshakeChannel>> Connect(const std::string& host,
            uint16_t port, int timeout_ms = 5000);

    // Server: accept connection
    static Result<std::unique_ptr<TcpHandshakeChannel>> Accept(int listen_fd,
            int timeout_ms = 5000);

    // Create listening socket
    static Result<int> CreateListenSocket(const std::string& bind_addr, uint16_t port,
            int backlog = 128);

    // HandshakeChannel interface
    Status Send(const HandshakeMessage& msg) override;
    Status Receive(HandshakeMessage& msg) override;
    void Close() override;
    bool IsConnected() const override;
    std::string GetPeerAddress() const override;

private:
    TcpHandshakeChannel() = default;
    explicit TcpHandshakeChannel(int fd);

    int fd_ = -1;
    std::string peer_address_;
};

// TCP Handshake Server for accepting multiple connections
class TcpHandshakeServer {
public:
    ~TcpHandshakeServer();

    // Non-copyable
    TcpHandshakeServer(const TcpHandshakeServer&) = delete;
    TcpHandshakeServer& operator=(const TcpHandshakeServer&) = delete;

    // Factory method
    static Result<std::unique_ptr<TcpHandshakeServer>> Create(const std::string& bind_addr,
            uint16_t port, int backlog = 128);

    // Accept a connection
    Result<std::unique_ptr<TcpHandshakeChannel>> Accept(int timeout_ms = -1);

    // Get listening port
    uint16_t GetPort() const { return port_; }

    // Close server
    void Close();

private:
    TcpHandshakeServer() = default;

    int listen_fd_ = -1;
    uint16_t port_ = 0;
};

}  // namespace rdma
