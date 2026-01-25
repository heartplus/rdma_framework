// RDMA KV Server
// High-performance key-value server using RDMA
// Supports GET and PUT operations with scatter-gather for PUT

#pragma once

#include <signal.h>

#include <cstring>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "kv_protocol.h"
#include "kv_store.h"
#include "rdma/rdma.h"

extern volatile bool g_running;

void SignalHandler(int sig);

class KVServer {
public:
    KVServer() = default;

    bool Initialize(uint16_t port);
    void Run();

private:
    // Posted buffer tracking
    struct PostedBuffer {
        rdma::Buffer buf;
        uint16_t conn_id;
    };

    // Per-connection context
    struct ConnectionContext {
        rdma::Connection* conn;
        uint64_t requests_processed;
        uint64_t gets_processed;
        uint64_t puts_processed;
    };

    void AcceptConnections();
    void PostRecvBuffers(rdma::Connection* conn, int count);
    void HandleCompletion(const ibv_wc& wc);
    void HandleRequest(uint16_t conn_id, const ibv_wc& wc);
    void ProcessRequest(rdma::Connection* conn, const void* request_data, size_t request_len,
                        ConnectionContext* ctx);
    void ProcessGet(rdma::Connection* conn, const kv::RequestHeader* header, ConnectionContext* ctx);
    void ProcessPut(rdma::Connection* conn, const kv::RequestHeader* header, const void* request_data,
                    size_t request_len, ConnectionContext* ctx);
    void SendErrorResponse(rdma::Connection* conn, uint64_t key, kv::Status status);
    void OnError(uint16_t conn_id, ibv_wc_status status);
    void PrintStats();

    // RDMA resources
    std::unique_ptr<rdma::RdmaDevice> device_;
    std::unique_ptr<rdma::ProtectionDomain> pd_;
    std::unique_ptr<rdma::CompletionQueue> send_cq_;
    std::unique_ptr<rdma::CompletionQueue> recv_cq_;
    std::unique_ptr<rdma::LocalBufferPool> buffer_pool_;
    std::unique_ptr<rdma::ConnectionManager> conn_manager_;
    std::unique_ptr<rdma::Reactor> reactor_;
    std::unique_ptr<rdma::TcpHandshakeServer> handshake_server_;
    rdma::CompletionDispatcher dispatcher_;

    // KV store
    kv::KVStore store_;

    // Connection contexts
    std::unordered_map<uint16_t, ConnectionContext*> conn_contexts_;

    // Buffer tracking for posted recv/send operations
    std::unordered_map<uint64_t, PostedBuffer> posted_recv_buffers_;
    std::unordered_map<uint64_t, rdma::Buffer> posted_send_buffers_;

    uint16_t port_ = 0;
};
