// RDMA Echo Server Example
// High-performance echo server using Reactor pattern

#include <signal.h>

#include <cstring>
#include <iostream>

#include "rdma/rdma.h"

using namespace rdma;

static volatile bool g_running = true;

void SignalHandler(int /*sig*/) {
    g_running = false;
}

class EchoServer {
public:
    EchoServer() = default;

    bool Initialize(uint16_t port) {
        // Open RDMA device
        auto device_result = RdmaDevice::Open();
        if (!device_result.ok()) {
            std::cerr << "Failed to open RDMA device" << std::endl;
            return false;
        }
        device_ = std::move(device_result.value());

        // Create protection domain
        auto pd_result = ProtectionDomain::Create(device_.get());
        if (!pd_result.ok()) {
            std::cerr << "Failed to create PD" << std::endl;
            return false;
        }
        pd_ = std::move(pd_result.value());

        // Create completion queues
        CompletionQueueConfig cq_config;
        cq_config.cq_depth = 8192;

        auto send_cq_result = CompletionQueue::Create(device_.get(), cq_config);
        auto recv_cq_result = CompletionQueue::Create(device_.get(), cq_config);
        if (!send_cq_result.ok() || !recv_cq_result.ok()) {
            std::cerr << "Failed to create CQs" << std::endl;
            return false;
        }
        send_cq_ = std::move(send_cq_result.value());
        recv_cq_ = std::move(recv_cq_result.value());

        // Create buffer pool
        BufferPoolConfig bp_config;
        bp_config.buffer_size = 4096;
        bp_config.buffer_count = 1024;
        bp_config.use_hugepage = false;

        auto bp_result = LocalBufferPool::Create(pd_.get(), bp_config);
        if (!bp_result.ok()) {
            std::cerr << "Failed to create buffer pool" << std::endl;
            return false;
        }
        buffer_pool_ = std::move(bp_result.value());

        // Create connection manager
        ConnectionManagerConfig cm_config;
        cm_config.max_connections = 256;

        auto cm_result = ConnectionManager::Create(pd_.get(), send_cq_.get(), recv_cq_.get(),
                cm_config);
        if (!cm_result.ok()) {
            std::cerr << "Failed to create connection manager" << std::endl;
            return false;
        }
        conn_manager_ = std::move(cm_result.value());

        // Create reactor
        ReactorConfig reactor_config;
        reactor_config.batch_mode = BatchMode::THROUGHPUT;

        auto reactor_result = Reactor::Create(reactor_config);
        if (!reactor_result.ok()) {
            std::cerr << "Failed to create reactor" << std::endl;
            return false;
        }
        reactor_ = std::move(reactor_result.value());

        // Register CQs with reactor
        reactor_->RegisterCQ(send_cq_.get(), &dispatcher_);
        reactor_->RegisterCQ(recv_cq_.get(), &dispatcher_);

        // Set error handler
        dispatcher_.SetErrorHandler(this,
                [](void* ctx, uint16_t conn_id, ibv_wc_status status) {
                    auto* self = static_cast<EchoServer*>(ctx);
                    self->OnError(conn_id, status);
                });

        // Create handshake server
        auto server_result = TcpHandshakeServer::Create("", port);
        if (!server_result.ok()) {
            std::cerr << "Failed to create handshake server" << std::endl;
            return false;
        }
        handshake_server_ = std::move(server_result.value());

        port_ = port;
        std::cout << "Echo server initialized on port " << port << std::endl;
        return true;
    }

    void Run() {
        std::cout << "Echo server running..." << std::endl;

        wc_buffer_.resize(64);

        while (g_running) {
            // Accept new connections (non-blocking)
            AcceptConnections();

            // Poll for completions
            PollCompletions();
        }
    }

private:
    void AcceptConnections() {
        auto channel_result = handshake_server_->Accept(0);  // Non-blocking
        if (!channel_result.ok()) {
            return;
        }

        auto& channel = channel_result.value();
        std::cout << "New connection from " << channel->GetPeerAddress() << std::endl;

        // Create connection
        auto conn_result = conn_manager_->CreateConnection();
        if (!conn_result.ok()) {
            std::cerr << "Failed to create connection" << std::endl;
            return;
        }
        auto* conn = conn_result.value();

        // Exchange parameters
        auto local_params_result = conn->GetLocalParams();
        if (!local_params_result.ok()) {
            conn_manager_->RemoveConnection(conn->GetConnId());
            return;
        }

        HandshakeMessage msg = {};
        msg.qp_params = local_params_result.value();
        msg.conn_id = conn->GetConnId();
        msg.version = 1;

        if (!channel->Send(msg).ok()) {
            conn_manager_->RemoveConnection(conn->GetConnId());
            return;
        }

        HandshakeMessage remote_msg;
        if (!channel->Receive(remote_msg).ok()) {
            conn_manager_->RemoveConnection(conn->GetConnId());
            return;
        }

        auto status = conn->Connect(remote_msg.qp_params);
        if (!status.ok()) {
            std::cerr << "Failed to connect: " << status.ToString() << std::endl;
            conn_manager_->RemoveConnection(conn->GetConnId());
            return;
        }

        // Register with dispatcher
        dispatcher_.RegisterConnection(conn->GetConnId(), conn,
                [](void* ctx, const ibv_wc& wc) {
                    auto* conn = static_cast<Connection*>(ctx);
                    // Echo the received data back
                    if (wc.status == IBV_WC_SUCCESS) {
                        RpcOpcode opcode = WrId::GetOpcode(wc.wr_id);
                        if (opcode == RpcOpcode::RECV) {
                            std::cout << "Echoing " << wc.byte_len << " bytes on conn "
                                      << conn->GetConnId() << std::endl;
                        }
                    }
                });

        // Post receive buffers
        for (int i = 0; i < 32; ++i) {
            Buffer buf = buffer_pool_->Allocate();
            if (buf.IsValid()) {
                uint64_t wr_id = WrId::Encode(conn->GetConnId(), RpcOpcode::RECV, 0, i);
                conn->PostRecv(buf, wr_id);
            }
        }

        std::cout << "Connection " << conn->GetConnId() << " established" << std::endl;
    }

    void PollCompletions() {
        int n = send_cq_->Poll(wc_buffer_.data(), wc_buffer_.size());
        for (int i = 0; i < n; ++i) {
            dispatcher_.Dispatch(wc_buffer_[i]);
        }

        n = recv_cq_->Poll(wc_buffer_.data(), wc_buffer_.size());
        for (int i = 0; i < n; ++i) {
            dispatcher_.Dispatch(wc_buffer_[i]);
        }
    }

    void OnError(uint16_t conn_id, ibv_wc_status status) {
        std::cerr << "Error on connection " << conn_id << ": " << ibv_wc_status_str(status)
                  << std::endl;
        conn_manager_->RemoveConnection(conn_id);
    }

    std::unique_ptr<RdmaDevice> device_;
    std::unique_ptr<ProtectionDomain> pd_;
    std::unique_ptr<CompletionQueue> send_cq_;
    std::unique_ptr<CompletionQueue> recv_cq_;
    std::unique_ptr<LocalBufferPool> buffer_pool_;
    std::unique_ptr<ConnectionManager> conn_manager_;
    std::unique_ptr<Reactor> reactor_;
    std::unique_ptr<TcpHandshakeServer> handshake_server_;
    CompletionDispatcher dispatcher_;

    uint16_t port_ = 0;
    std::vector<ibv_wc> wc_buffer_;
};

int main(int argc, char* argv[]) {
    uint16_t port = 12345;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    EchoServer server;
    if (!server.Initialize(port)) {
        return 1;
    }

    server.Run();

    std::cout << "Echo server stopped" << std::endl;
    return 0;
}
