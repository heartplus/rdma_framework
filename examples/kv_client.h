// RDMA KV Client
// Benchmark client for KV operations
// Supports GET and PUT with configurable ratio and scatter-gather PUT

#pragma once

#include <signal.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "kv_protocol.h"
#include "rdma/rdma.h"

extern volatile bool g_running;

void SignalHandler(int sig);

class KVClient {
public:
    KVClient() = default;

    bool Connect(const std::string& host, uint16_t port);

    // Run KV benchmark with specified GET/PUT ratio
    // put_ratio: 0.0 = all GETs, 1.0 = all PUTs
    // use_sgl: if true, PUT operations use scatter-gather (non-contiguous memory)
    void RunBenchmark(int num_operations, float put_ratio, uint32_t value_size, bool use_sgl,
                      int num_segments);

private:
    void PostRecvBuffers(size_t count);
    void SendGet(uint64_t key, uint32_t seq);
    void SendPut(uint64_t key, uint32_t value_size, uint32_t seq);
    // Send PUT with scatter-gather (non-contiguous memory simulation)
    void SendPutSGL(uint64_t key, uint32_t value_size, int num_segments, uint32_t seq);

    std::unique_ptr<rdma::RdmaDevice> device_;
    std::unique_ptr<rdma::ProtectionDomain> pd_;
    std::unique_ptr<rdma::CompletionQueue> send_cq_;
    std::unique_ptr<rdma::CompletionQueue> recv_cq_;
    std::unique_ptr<rdma::LocalBufferPool> buffer_pool_;
    std::unique_ptr<rdma::Connection> conn_;

    // Buffer tracking
    std::unordered_map<uint64_t, rdma::Buffer> posted_recv_buffers_;
    std::unordered_map<uint64_t, rdma::Buffer> posted_send_buffers_;
};
