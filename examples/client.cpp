// RDMA Framework Example Client
// Simple client that connects to server and sends messages

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
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <server_host> [port]" << std::endl;
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = 12345;
    if (argc > 2) {
        port = static_cast<uint16_t>(std::atoi(argv[2]));
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    std::cout << "Connecting to " << host << ":" << port << std::endl;

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
    bp_config.use_hugepage = false;

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

    // Connect via TCP for handshake
    auto channel_result = TcpHandshakeChannel::Connect(host, port);
    if (!channel_result.ok()) {
        std::cerr << "Failed to connect to server: " << channel_result.status().ToString()
                  << std::endl;
        return 1;
    }
    auto& channel = channel_result.value();

    // Create connection
    ConnectionConfig conn_config;
    auto conn_result = Connection::Create(pd.get(), send_cq.get(), recv_cq.get(), conn_config);
    if (!conn_result.ok()) {
        std::cerr << "Failed to create connection" << std::endl;
        return 1;
    }
    auto& conn = conn_result.value();

    // Exchange QP parameters
    auto local_params_result = conn->GetLocalParams();
    if (!local_params_result.ok()) {
        std::cerr << "Failed to get local params" << std::endl;
        return 1;
    }

    // Receive server's params first
    HandshakeMessage remote_msg;
    auto status = channel->Receive(remote_msg);
    if (!status.ok()) {
        std::cerr << "Failed to receive handshake: " << status.ToString() << std::endl;
        return 1;
    }

    // Send our params
    HandshakeMessage msg = {};
    msg.qp_params = local_params_result.value();
    msg.version = 1;

    status = channel->Send(msg);
    if (!status.ok()) {
        std::cerr << "Failed to send handshake: " << status.ToString() << std::endl;
        return 1;
    }

    // Connect
    status = conn->Connect(remote_msg.qp_params);
    if (!status.ok()) {
        std::cerr << "Failed to connect: " << status.ToString() << std::endl;
        return 1;
    }

    std::cout << "Connected to server" << std::endl;

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

    // Send messages
    const char* message = "Hello, RDMA!";
    int message_count = 0;

    std::vector<ibv_wc> wc_buffer(32);

    while (g_running && conn->GetState() == ConnectionState::CONNECTED) {
        // Send a message every second
        static auto last_send = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_send).count() >= 1) {
            Buffer buf = buffer_pool->Allocate();
            if (buf.IsValid()) {
                memcpy(buf.GetAddr(), message, strlen(message) + 1);
                uint64_t wr_id = WrId::Encode(conn->GetConnId(), RpcOpcode::REQUEST_SEND, 0,
                        message_count++);
                status = conn->Send(buf, wr_id);
                if (status.ok()) {
                    std::cout << "Sent message #" << message_count << std::endl;
                }
            }
            last_send = now;
        }

        // Poll for completions
        int n = send_cq->Poll(wc_buffer.data(), wc_buffer.size());
        for (int i = 0; i < n; ++i) {
            dispatcher.Dispatch(wc_buffer[i]);
        }

        n = recv_cq->Poll(wc_buffer.data(), wc_buffer.size());
        for (int i = 0; i < n; ++i) {
            dispatcher.Dispatch(wc_buffer[i]);
        }

        // Small sleep to avoid busy loop
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "Client shutting down" << std::endl;
    return 0;
}
