// RDMA Framework Example Server
// Simple server that accepts connections and echoes messages

#include <signal.h>

#include <cstring>
#include <iostream>

#include "rdma/rdma.h"

using namespace rdma;

static volatile bool g_running = true;

void SignalHandler(int /*sig*/) {
    g_running = false;
}

// WC handler for completions
void OnCompletion(void* ctx, const ibv_wc& wc) {
    auto* conn = static_cast<Connection*>(ctx);
    (void)conn;

    if (wc.status != IBV_WC_SUCCESS) {
        std::cerr << "WC error: " << ibv_wc_status_str(wc.status) << std::endl;
        return;
    }

    RpcOpcode opcode = WrId::GetOpcode(wc.wr_id);

    switch (opcode) {
        case RpcOpcode::RECV:
            std::cout << "Received " << wc.byte_len << " bytes" << std::endl;
            break;
        case RpcOpcode::REQUEST_SEND:
        case RpcOpcode::RESPONSE_SEND:
            std::cout << "Send completed" << std::endl;
            break;
        default:
            break;
    }
}

void OnError(void* /*ctx*/, uint16_t conn_id, ibv_wc_status status) {
    std::cerr << "Error on connection " << conn_id << ": " << ibv_wc_status_str(status)
              << std::endl;
}

int main(int argc, char* argv[]) {
    uint16_t port = 12345;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    std::cout << "Starting RDMA server on port " << port << std::endl;

    // Open RDMA device
    auto device_result = RdmaDevice::Open();
    if (!device_result.ok()) {
        std::cerr << "Failed to open RDMA device: " << device_result.status().ToString()
                  << std::endl;
        return 1;
    }
    auto& device = device_result.value();
    std::cout << "Opened device: " << device->GetName() << std::endl;

    // Create protection domain
    auto pd_result = ProtectionDomain::Create(device.get());
    if (!pd_result.ok()) {
        std::cerr << "Failed to create PD: " << pd_result.status().ToString() << std::endl;
        return 1;
    }
    auto& pd = pd_result.value();

    // Create completion queues
    CompletionQueueConfig cq_config;
    cq_config.cq_depth = 4096;

    auto send_cq_result = CompletionQueue::Create(device.get(), cq_config);
    auto recv_cq_result = CompletionQueue::Create(device.get(), cq_config);
    if (!send_cq_result.ok() || !recv_cq_result.ok()) {
        std::cerr << "Failed to create CQs" << std::endl;
        return 1;
    }
    auto& send_cq = send_cq_result.value();
    auto& recv_cq = recv_cq_result.value();

    // Create buffer pool
    BufferPoolConfig bp_config;
    bp_config.buffer_size = 4096;
    bp_config.buffer_count = 256;
    bp_config.use_hugepage = false;  // Use regular pages for simplicity

    auto bp_result = LocalBufferPool::Create(pd.get(), bp_config);
    if (!bp_result.ok()) {
        std::cerr << "Failed to create buffer pool: " << bp_result.status().ToString()
                  << std::endl;
        return 1;
    }
    auto& buffer_pool = bp_result.value();

    // Create completion dispatcher
    CompletionDispatcher dispatcher;
    dispatcher.SetErrorHandler(nullptr, OnError);

    // Create TCP handshake server
    auto server_result = TcpHandshakeServer::Create("", port);
    if (!server_result.ok()) {
        std::cerr << "Failed to create handshake server: " << server_result.status().ToString()
                  << std::endl;
        return 1;
    }
    auto& server = server_result.value();

    std::cout << "Server listening on port " << port << std::endl;

    // Pre-allocate WC buffer for polling
    std::vector<ibv_wc> wc_buffer(32);

    while (g_running) {
        // Accept new connections
        auto channel_result = server->Accept(100);  // 100ms timeout
        if (channel_result.ok()) {
            auto& channel = channel_result.value();
            std::cout << "Accepted connection from " << channel->GetPeerAddress() << std::endl;

            // Create connection
            ConnectionConfig conn_config;
            auto conn_result = Connection::Create(pd.get(), send_cq.get(), recv_cq.get(),
                    conn_config);
            if (!conn_result.ok()) {
                std::cerr << "Failed to create connection" << std::endl;
                continue;
            }
            auto& conn = conn_result.value();

            // Exchange QP parameters
            auto local_params_result = conn->GetLocalParams();
            if (!local_params_result.ok()) {
                std::cerr << "Failed to get local params" << std::endl;
                continue;
            }

            HandshakeMessage msg = {};
            msg.qp_params = local_params_result.value();
            msg.version = 1;

            // Send our params
            auto status = channel->Send(msg);
            if (!status.ok()) {
                std::cerr << "Failed to send handshake" << std::endl;
                continue;
            }

            // Receive remote params
            HandshakeMessage remote_msg;
            status = channel->Receive(remote_msg);
            if (!status.ok()) {
                std::cerr << "Failed to receive handshake" << std::endl;
                continue;
            }

            // Connect
            status = conn->Connect(remote_msg.qp_params);
            if (!status.ok()) {
                std::cerr << "Failed to connect: " << status.ToString() << std::endl;
                continue;
            }

            std::cout << "Connection established with QP " << remote_msg.qp_params.qp_num
                      << std::endl;

            // Register connection with dispatcher
            dispatcher.RegisterConnection(conn->GetConnId(), conn.get(), OnCompletion);

            // Post receive buffers
            for (int i = 0; i < 16; ++i) {
                Buffer buf = buffer_pool->Allocate();
                if (buf.IsValid()) {
                    uint64_t wr_id = WrId::Encode(conn->GetConnId(), RpcOpcode::RECV, 0, i);
                    conn->PostRecv(buf, wr_id);
                }
            }

            // Simple message loop (for demo)
            // In production, this would be in the Reactor
            while (g_running && conn->GetState() == ConnectionState::CONNECTED) {
                int n = send_cq->Poll(wc_buffer.data(), wc_buffer.size());
                for (int i = 0; i < n; ++i) {
                    dispatcher.Dispatch(wc_buffer[i]);
                }

                n = recv_cq->Poll(wc_buffer.data(), wc_buffer.size());
                for (int i = 0; i < n; ++i) {
                    dispatcher.Dispatch(wc_buffer[i]);
                }
            }
        }
    }

    std::cout << "Server shutting down" << std::endl;
    return 0;
}
