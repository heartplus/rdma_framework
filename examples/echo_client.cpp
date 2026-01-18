// RDMA Echo Client Example
// Sends messages to echo server and measures latency

#include <signal.h>

#include <chrono>
#include <cstring>
#include <iostream>

#include "rdma/rdma.h"

using namespace rdma;

static volatile bool g_running = true;

void SignalHandler(int /*sig*/) {
    g_running = false;
}

class EchoClient {
public:
    EchoClient() = default;

    bool Connect(const std::string& host, uint16_t port) {
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
        cq_config.cq_depth = 4096;

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
        bp_config.buffer_count = 256;
        bp_config.use_hugepage = false;

        auto bp_result = LocalBufferPool::Create(pd_.get(), bp_config);
        if (!bp_result.ok()) {
            std::cerr << "Failed to create buffer pool" << std::endl;
            return false;
        }
        buffer_pool_ = std::move(bp_result.value());

        // Connect via TCP for handshake
        auto channel_result = TcpHandshakeChannel::Connect(host, port);
        if (!channel_result.ok()) {
            std::cerr << "Failed to connect to server" << std::endl;
            return false;
        }
        auto& channel = channel_result.value();

        // Create RDMA connection
        ConnectionConfig conn_config;
        auto conn_result = Connection::Create(pd_.get(), send_cq_.get(), recv_cq_.get(),
                conn_config);
        if (!conn_result.ok()) {
            std::cerr << "Failed to create connection" << std::endl;
            return false;
        }
        conn_ = std::move(conn_result.value());

        // Exchange parameters
        auto local_params_result = conn_->GetLocalParams();
        if (!local_params_result.ok()) {
            std::cerr << "Failed to get local params" << std::endl;
            return false;
        }

        // Receive server's params first
        HandshakeMessage remote_msg;
        if (!channel->Receive(remote_msg).ok()) {
            std::cerr << "Failed to receive handshake" << std::endl;
            return false;
        }

        // Send our params
        HandshakeMessage msg = {};
        msg.qp_params = local_params_result.value();
        msg.version = 1;

        if (!channel->Send(msg).ok()) {
            std::cerr << "Failed to send handshake" << std::endl;
            return false;
        }

        // Connect QP
        auto status = conn_->Connect(remote_msg.qp_params);
        if (!status.ok()) {
            std::cerr << "Failed to connect QP: " << status.ToString() << std::endl;
            return false;
        }

        // Post receive buffers
        for (int i = 0; i < 32; ++i) {
            Buffer buf = buffer_pool_->Allocate();
            if (buf.IsValid()) {
                uint64_t wr_id = WrId::Encode(conn_->GetConnId(), RpcOpcode::RECV, 0, i);
                conn_->PostRecv(buf, wr_id);
            }
        }

        std::cout << "Connected to server" << std::endl;
        return true;
    }

    void RunBenchmark(int num_messages, int message_size) {
        std::cout << "Starting benchmark: " << num_messages << " messages, " << message_size
                  << " bytes each" << std::endl;

        std::vector<ibv_wc> wc_buffer(64);
        int sent = 0;
        int completed = 0;

        auto start = std::chrono::high_resolution_clock::now();

        while (g_running && completed < num_messages) {
            // Send messages
            while (sent < num_messages && sent - completed < 64) {
                Buffer buf = buffer_pool_->Allocate();
                if (buf.IsValid()) {
                    // Fill with pattern
                    memset(buf.GetAddr(), 'A' + (sent % 26), message_size);

                    uint64_t wr_id = WrId::Encode(conn_->GetConnId(), RpcOpcode::REQUEST_SEND,
                            0, sent);
                    auto status = conn_->Send(buf.Slice(0, message_size), wr_id);
                    if (status.ok()) {
                        ++sent;
                    }
                }
            }

            // Poll for send completions
            int n = send_cq_->Poll(wc_buffer.data(), wc_buffer.size());
            for (int i = 0; i < n; ++i) {
                if (wc_buffer[i].status == IBV_WC_SUCCESS) {
                    ++completed;
                } else {
                    std::cerr << "Send error: " << ibv_wc_status_str(wc_buffer[i].status)
                              << std::endl;
                }
            }

            // Poll for receive completions (echo responses)
            n = recv_cq_->Poll(wc_buffer.data(), wc_buffer.size());
            for (int i = 0; i < n; ++i) {
                if (wc_buffer[i].status != IBV_WC_SUCCESS) {
                    std::cerr << "Recv error: " << ibv_wc_status_str(wc_buffer[i].status)
                              << std::endl;
                }
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        double seconds = duration / 1000000.0;
        double msg_per_sec = completed / seconds;
        double bandwidth_mbps = (completed * message_size * 8.0) / (seconds * 1000000.0);
        double avg_latency_us = duration / (double)completed;

        std::cout << "\n=== Benchmark Results ===" << std::endl;
        std::cout << "Messages sent:    " << completed << std::endl;
        std::cout << "Time:             " << seconds << " seconds" << std::endl;
        std::cout << "Throughput:       " << msg_per_sec << " msg/sec" << std::endl;
        std::cout << "Bandwidth:        " << bandwidth_mbps << " Mbps" << std::endl;
        std::cout << "Avg latency:      " << avg_latency_us << " us" << std::endl;
    }

private:
    std::unique_ptr<RdmaDevice> device_;
    std::unique_ptr<ProtectionDomain> pd_;
    std::unique_ptr<CompletionQueue> send_cq_;
    std::unique_ptr<CompletionQueue> recv_cq_;
    std::unique_ptr<LocalBufferPool> buffer_pool_;
    std::unique_ptr<Connection> conn_;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <server_host> [port] [num_messages] [msg_size]"
                  << std::endl;
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = 12345;
    int num_messages = 10000;
    int message_size = 64;

    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 3) num_messages = std::atoi(argv[3]);
    if (argc > 4) message_size = std::atoi(argv[4]);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    EchoClient client;
    if (!client.Connect(host, port)) {
        return 1;
    }

    client.RunBenchmark(num_messages, message_size);

    std::cout << "Client finished" << std::endl;
    return 0;
}
