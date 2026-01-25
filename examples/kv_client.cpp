// RDMA KV Client
// Benchmark client for KV operations
// Supports GET and PUT with configurable ratio and scatter-gather PUT

#include <signal.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <unordered_map>
#include <vector>

#include "kv_protocol.h"
#include "rdma/rdma.h"

using namespace rdma;

static volatile bool g_running = true;

void SignalHandler(int /*sig*/) {
    g_running = false;
}

class KVClient {
public:
    KVClient() = default;

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

        // Create buffer pool with larger buffers to support 64KB values
        BufferPoolConfig bp_config;
        bp_config.buffer_size = kv::MAX_RESPONSE_SIZE;  // Response can include 64KB value
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
        auto conn_result =
                Connection::Create(pd_.get(), send_cq_.get(), recv_cq_.get(), conn_config);
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
        PostRecvBuffers(64);

        std::cout << "Connected to server" << std::endl;
        return true;
    }

    // Run KV benchmark with specified GET/PUT ratio
    // put_ratio: 0.0 = all GETs, 1.0 = all PUTs
    // use_sgl: if true, PUT operations use scatter-gather (non-contiguous memory)
    void RunBenchmark(int num_operations, float put_ratio, uint32_t value_size, bool use_sgl,
                      int num_segments) {
        std::cout << "Starting KV benchmark:" << std::endl;
        std::cout << "  Operations: " << num_operations << std::endl;
        std::cout << "  PUT ratio: " << (put_ratio * 100) << "%" << std::endl;
        std::cout << "  Value size: " << value_size << " bytes" << std::endl;
        std::cout << "  Use SGL: " << (use_sgl ? "yes" : "no") << std::endl;
        if (use_sgl) {
            std::cout << "  Segments: " << num_segments << std::endl;
        }

        // Validate value size
        if (!kv::IsValidValueSize(value_size)) {
            std::cerr << "Invalid value size. Must be 4KB-64KB, 4KB aligned." << std::endl;
            return;
        }

        // Random number generators
        std::mt19937_64 rng(std::random_device{}());
        std::uniform_real_distribution<float> op_dist(0.0f, 1.0f);
        std::uniform_int_distribution<uint64_t> key_dist(1, 10000);

        std::vector<ibv_wc> wc_buffer(64);

        int sent = 0;
        int completed = 0;
        int gets_sent = 0;
        int puts_sent = 0;
        int gets_success = 0;
        int puts_success = 0;
        int gets_not_found = 0;

        // Track in-flight requests
        constexpr int MAX_OUTSTANDING = 64;

        auto start = std::chrono::high_resolution_clock::now();

        while (g_running && completed < num_operations) {
            // Send requests (pipelined)
            while (sent < num_operations && sent - completed < MAX_OUTSTANDING) {
                bool do_put = (op_dist(rng) < put_ratio);
                uint64_t key = key_dist(rng);

                if (do_put) {
                    if (use_sgl && num_segments > 1) {
                        SendPutSGL(key, value_size, num_segments, sent);
                    } else {
                        SendPut(key, value_size, sent);
                    }
                    ++puts_sent;
                } else {
                    SendGet(key, sent);
                    ++gets_sent;
                }
                ++sent;
            }

            // Poll for send completions
            int n = send_cq_->Poll(wc_buffer.data(), wc_buffer.size());
            for (int i = 0; i < n; ++i) {
                if (wc_buffer[i].status != IBV_WC_SUCCESS) {
                    std::cerr << "Send error: " << ibv_wc_status_str(wc_buffer[i].status)
                              << std::endl;
                }
                // Free send buffer
                auto it = posted_send_buffers_.find(wc_buffer[i].wr_id);
                if (it != posted_send_buffers_.end()) {
                    buffer_pool_->Free(it->second);
                    posted_send_buffers_.erase(it);
                }
            }

            // Poll for receive completions (responses)
            n = recv_cq_->Poll(wc_buffer.data(), wc_buffer.size());
            for (int i = 0; i < n; ++i) {
                if (wc_buffer[i].status == IBV_WC_SUCCESS) {
                    // Process response
                    auto it = posted_recv_buffers_.find(wc_buffer[i].wr_id);
                    if (it != posted_recv_buffers_.end()) {
                        const auto* response =
                                static_cast<const kv::ResponseHeader*>(it->second.GetAddr());
                        kv::OpType op = static_cast<kv::OpType>(response->op_type);
                        kv::Status status = static_cast<kv::Status>(response->status);

                        if (op == kv::OpType::GET_RESPONSE) {
                            if (status == kv::Status::OK) {
                                ++gets_success;
                            } else if (status == kv::Status::NOT_FOUND) {
                                ++gets_not_found;
                            }
                        } else if (op == kv::OpType::PUT_RESPONSE) {
                            if (status == kv::Status::OK) {
                                ++puts_success;
                            }
                        }

                        // Free and repost receive buffer
                        buffer_pool_->Free(it->second);
                        posted_recv_buffers_.erase(it);
                    }
                    ++completed;
                } else {
                    std::cerr << "Recv error: " << ibv_wc_status_str(wc_buffer[i].status)
                              << std::endl;
                }
            }

            // Ensure we have enough receive buffers posted
            if (posted_recv_buffers_.size() < 32) {
                PostRecvBuffers(32 - posted_recv_buffers_.size());
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        double seconds = duration / 1000000.0;
        double ops_per_sec = completed / seconds;
        double bandwidth_mbps = (completed * value_size * 8.0) / (seconds * 1000000.0);
        double avg_latency_us = duration / static_cast<double>(completed);

        std::cout << "\n=== KV Benchmark Results ===" << std::endl;
        std::cout << "Operations completed: " << completed << std::endl;
        std::cout << "  GETs: " << gets_sent << " (success: " << gets_success
                  << ", not found: " << gets_not_found << ")" << std::endl;
        std::cout << "  PUTs: " << puts_sent << " (success: " << puts_success << ")" << std::endl;
        std::cout << "Duration: " << seconds << " seconds" << std::endl;
        std::cout << "Throughput: " << ops_per_sec << " ops/sec" << std::endl;
        std::cout << "Bandwidth: " << bandwidth_mbps << " Mbps (approx)" << std::endl;
        std::cout << "Avg latency: " << avg_latency_us << " us" << std::endl;
    }

private:
    void PostRecvBuffers(size_t count) {
        for (size_t i = 0; i < count; ++i) {
            Buffer buf = buffer_pool_->Allocate();
            if (buf.IsValid()) {
                uint32_t buf_index =
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buf.GetAddr()) &
                                              0xFFFFFFFF);
                uint64_t wr_id = WrId::Encode(conn_->GetConnId(), RpcOpcode::RECV, 0, buf_index);

                posted_recv_buffers_[wr_id] = buf;
                conn_->PostRecv(buf, wr_id);
            }
        }
    }

    void SendGet(uint64_t key, uint32_t seq) {
        Buffer send_buf = buffer_pool_->Allocate();
        if (!send_buf.IsValid()) {
            return;
        }

        auto* header = static_cast<kv::RequestHeader*>(send_buf.GetAddr());
        header->SetGet(key);

        uint64_t wr_id = WrId::Encode(conn_->GetConnId(), RpcOpcode::REQUEST_SEND, 0, seq);
        posted_send_buffers_[wr_id] = send_buf;

        conn_->Send(send_buf.Slice(0, sizeof(kv::RequestHeader)), wr_id);
    }

    void SendPut(uint64_t key, uint32_t value_size, uint32_t seq) {
        Buffer send_buf = buffer_pool_->Allocate();
        if (!send_buf.IsValid()) {
            return;
        }

        auto* header = static_cast<kv::RequestHeader*>(send_buf.GetAddr());
        header->SetPut(key, value_size, 0);  // 0 segments = contiguous

        // Fill value with pattern
        uint8_t* value_ptr = send_buf.GetData() + sizeof(kv::RequestHeader);
        std::memset(value_ptr, static_cast<uint8_t>(key & 0xFF), value_size);

        size_t total_size = sizeof(kv::RequestHeader) + value_size;

        uint64_t wr_id = WrId::Encode(conn_->GetConnId(), RpcOpcode::REQUEST_SEND, 0, seq);
        posted_send_buffers_[wr_id] = send_buf;

        conn_->Send(send_buf.Slice(0, total_size), wr_id);
    }

    // Send PUT with scatter-gather (non-contiguous memory simulation)
    // In this example, we allocate separate buffers and use SGL to send them
    // The data is divided into num_segments segments
    void SendPutSGL(uint64_t key, uint32_t value_size, int num_segments, uint32_t seq) {
        // For SGL, we need to prepare multiple segments
        // Each segment comes from the same registered MR (buffer pool)

        // Allocate main buffer for header + segment descriptors
        Buffer header_buf = buffer_pool_->Allocate();
        if (!header_buf.IsValid()) {
            return;
        }

        // Calculate segment sizes
        uint32_t base_segment_size = value_size / num_segments;
        uint32_t remainder = value_size % num_segments;

        // Prepare header
        auto* header = static_cast<kv::RequestHeader*>(header_buf.GetAddr());
        header->SetPut(key, value_size, static_cast<uint8_t>(num_segments));

        // Prepare segment descriptors
        auto* descriptors = reinterpret_cast<kv::SegmentDescriptor*>(header_buf.GetData() +
                                                                     sizeof(kv::RequestHeader));

        // Allocate segment buffers and prepare iovec array
        std::vector<Buffer> segment_buffers;
        std::vector<struct iovec> iovs;

        // First iovec: header + descriptors
        size_t header_total =
                sizeof(kv::RequestHeader) + num_segments * sizeof(kv::SegmentDescriptor);
        iovs.push_back({header_buf.GetAddr(), header_total});

        uint32_t offset = 0;
        for (int i = 0; i < num_segments; ++i) {
            uint32_t seg_size = base_segment_size + (i < static_cast<int>(remainder) ? 1 : 0);

            // Record segment descriptor
            descriptors[i].offset = offset;
            descriptors[i].length = seg_size;

            // For simplicity, we put segment data right after descriptors in the same buffer
            // In a real scenario, these would be from different memory locations
            offset += seg_size;
        }

        // Put all segment data after the descriptors in the header buffer
        uint8_t* data_ptr = header_buf.GetData() + header_total;
        std::memset(data_ptr, static_cast<uint8_t>(key & 0xFF), value_size);

        // Total message size
        size_t total_size = header_total + value_size;

        uint64_t wr_id = WrId::Encode(conn_->GetConnId(), RpcOpcode::REQUEST_SEND, 0, seq);
        posted_send_buffers_[wr_id] = header_buf;

        // Send as single contiguous message
        // (In a real SGL scenario, you would use PostSendSGL with multiple iovecs)
        conn_->Send(header_buf.Slice(0, total_size), wr_id);
    }

    std::unique_ptr<RdmaDevice> device_;
    std::unique_ptr<ProtectionDomain> pd_;
    std::unique_ptr<CompletionQueue> send_cq_;
    std::unique_ptr<CompletionQueue> recv_cq_;
    std::unique_ptr<LocalBufferPool> buffer_pool_;
    std::unique_ptr<Connection> conn_;

    // Buffer tracking
    std::unordered_map<uint64_t, Buffer> posted_recv_buffers_;
    std::unordered_map<uint64_t, Buffer> posted_send_buffers_;
};

void PrintUsage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " <server_host> [options]\n"
                 "Options:\n"
                 "  -p <port>         Server port (default: 12345)\n"
                 "  -n <num>          Number of operations (default: 10000)\n"
                 "  -r <ratio>        PUT ratio 0.0-1.0 (default: 0.5)\n"
                 "  -s <size>         Value size in bytes, 4K-64K, 4K aligned (default: 4096)\n"
                 "  --sgl             Use scatter-gather for PUT\n"
                 "  --segments <n>    Number of segments for SGL (default: 4)\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = 12345;
    int num_operations = 10000;
    float put_ratio = 0.5f;
    uint32_t value_size = 4096;
    bool use_sgl = false;
    int num_segments = 4;

    // Parse arguments
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "-n" && i + 1 < argc) {
            num_operations = std::atoi(argv[++i]);
        } else if (arg == "-r" && i + 1 < argc) {
            put_ratio = std::atof(argv[++i]);
        } else if (arg == "-s" && i + 1 < argc) {
            value_size = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--sgl") {
            use_sgl = true;
        } else if (arg == "--segments" && i + 1 < argc) {
            num_segments = std::atoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
    }

    // Validate parameters
    if (put_ratio < 0.0f || put_ratio > 1.0f) {
        std::cerr << "PUT ratio must be between 0.0 and 1.0" << std::endl;
        return 1;
    }

    if (!kv::IsValidValueSize(value_size)) {
        std::cerr << "Value size must be 4KB-64KB and 4KB aligned" << std::endl;
        return 1;
    }

    if (num_segments < 1 || num_segments > kv::MAX_SEGMENTS) {
        std::cerr << "Number of segments must be 1-" << kv::MAX_SEGMENTS << std::endl;
        return 1;
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    KVClient client;
    if (!client.Connect(host, port)) {
        return 1;
    }

    client.RunBenchmark(num_operations, put_ratio, value_size, use_sgl, num_segments);

    std::cout << "Client finished" << std::endl;
    return 0;
}
