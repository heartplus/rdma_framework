#include "rdma/transport/tcp_handshake.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "rdma/common/logging.h"

namespace rdma {

TcpHandshakeChannel::TcpHandshakeChannel(int fd) : fd_(fd) {}

TcpHandshakeChannel::~TcpHandshakeChannel() {
    Close();
}

Result<std::unique_ptr<TcpHandshakeChannel>> TcpHandshakeChannel::Connect(const std::string& host,
        uint16_t port, int timeout_ms) {
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result;
    std::string port_str = std::to_string(port);

    int ret = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (ret != 0) {
        return Status::IOError("Failed to resolve host");
    }

    int fd = -1;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }

        // Set non-blocking for timeout
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        ret = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (ret < 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }

        if (ret < 0) {
            // Wait for connection with timeout
            struct pollfd pfd = {};
            pfd.fd = fd;
            pfd.events = POLLOUT;

            ret = poll(&pfd, 1, timeout_ms);
            if (ret <= 0) {
                close(fd);
                fd = -1;
                continue;
            }

            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
            if (error != 0) {
                close(fd);
                fd = -1;
                continue;
            }
        }

        // Reset to blocking
        fcntl(fd, F_SETFL, flags);
        break;
    }

    freeaddrinfo(result);

    if (fd < 0) {
        return Status::ConnectionRefused("Failed to connect");
    }

    // Set TCP options
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    auto channel = std::unique_ptr<TcpHandshakeChannel>(new TcpHandshakeChannel(fd));
    channel->peer_address_ = host + ":" + std::to_string(port);

    RDMA_LOG_DEBUG("Connected to %s", channel->peer_address_.c_str());

    return channel;
}

Result<std::unique_ptr<TcpHandshakeChannel>> TcpHandshakeChannel::Accept(int listen_fd,
        int timeout_ms) {
    struct pollfd pfd = {};
    pfd.fd = listen_fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        return Status::IOError("poll failed", errno);
    }
    if (ret == 0) {
        return Status::Timeout("Accept timeout");
    }

    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);

    int fd = accept(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len);
    if (fd < 0) {
        return Status::IOError("accept failed", errno);
    }

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    auto channel = std::unique_ptr<TcpHandshakeChannel>(new TcpHandshakeChannel(fd));

    // Get peer address
    char host[NI_MAXHOST], service[NI_MAXSERV];
    if (getnameinfo(reinterpret_cast<struct sockaddr*>(&addr), addr_len, host, sizeof(host),
                service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        channel->peer_address_ = std::string(host) + ":" + service;
    }

    RDMA_LOG_DEBUG("Accepted connection from %s", channel->peer_address_.c_str());

    return channel;
}

Result<int> TcpHandshakeChannel::CreateListenSocket(const std::string& bind_addr, uint16_t port,
        int backlog) {
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* result;
    std::string port_str = std::to_string(port);

    int ret = getaddrinfo(bind_addr.empty() ? nullptr : bind_addr.c_str(), port_str.c_str(), &hints,
            &result);
    if (ret != 0) {
        return Status::IOError("Failed to resolve bind address");
    }

    int fd = -1;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);

    if (fd < 0) {
        return Status::IOError("Failed to bind socket");
    }

    if (listen(fd, backlog) < 0) {
        close(fd);
        return Status::IOError("Failed to listen", errno);
    }

    RDMA_LOG_INFO("Listening on port %u", port);

    return fd;
}

Status TcpHandshakeChannel::Send(const HandshakeMessage& msg) {
    if (fd_ < 0) {
        return Status::InvalidArgument("Channel not connected");
    }

    ssize_t sent = 0;
    size_t total = sizeof(msg);
    const char* data = reinterpret_cast<const char*>(&msg);

    while (sent < static_cast<ssize_t>(total)) {
        ssize_t n = write(fd_, data + sent, total - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::IOError("write failed", errno);
        }
        sent += n;
    }

    return Status::OK();
}

Status TcpHandshakeChannel::Receive(HandshakeMessage& msg) {
    if (fd_ < 0) {
        return Status::InvalidArgument("Channel not connected");
    }

    ssize_t received = 0;
    size_t total = sizeof(msg);
    char* data = reinterpret_cast<char*>(&msg);

    while (received < static_cast<ssize_t>(total)) {
        ssize_t n = read(fd_, data + received, total - received);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::IOError("read failed", errno);
        }
        if (n == 0) {
            return Status::ConnectionReset("Connection closed");
        }
        received += n;
    }

    return Status::OK();
}

void TcpHandshakeChannel::Close() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

bool TcpHandshakeChannel::IsConnected() const {
    return fd_ >= 0;
}

std::string TcpHandshakeChannel::GetPeerAddress() const {
    return peer_address_;
}

// TcpHandshakeServer implementation
TcpHandshakeServer::~TcpHandshakeServer() {
    Close();
}

Result<std::unique_ptr<TcpHandshakeServer>> TcpHandshakeServer::Create(const std::string& bind_addr,
        uint16_t port, int backlog) {
    auto fd_result = TcpHandshakeChannel::CreateListenSocket(bind_addr, port, backlog);
    if (!fd_result.ok()) {
        return fd_result.status();
    }

    auto server = std::unique_ptr<TcpHandshakeServer>(new TcpHandshakeServer());
    server->listen_fd_ = fd_result.value();
    server->port_ = port;

    return server;
}

Result<std::unique_ptr<TcpHandshakeChannel>> TcpHandshakeServer::Accept(int timeout_ms) {
    return TcpHandshakeChannel::Accept(listen_fd_, timeout_ms);
}

void TcpHandshakeServer::Close() {
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

}  // namespace rdma
