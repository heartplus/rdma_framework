// RDMA KV Client main entry point

#include <gflags/gflags.h>
#include <signal.h>

#include <iostream>

#include "kv_client.h"

DEFINE_string(host, "", "Server host to connect to");
DEFINE_int32(port, 12345, "Server port");
DEFINE_int32(num_operations, 10000, "Number of operations to run");
DEFINE_double(put_ratio, 0.5, "PUT ratio 0.0-1.0 (0.0 = all GETs, 1.0 = all PUTs)");
DEFINE_int32(value_size, 4096, "Value size in bytes, 4K-64K, 4K aligned");
DEFINE_bool(sgl, false, "Use scatter-gather for PUT operations");
DEFINE_int32(segments, 4, "Number of segments for SGL (1-16)");

int main(int argc, char* argv[]) {
    gflags::SetUsageMessage(
            "RDMA KV Client - Benchmark client for RDMA KV operations\n"
            "Usage: kv_client --host=<server_host> [options]");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // Validate required arguments
    if (FLAGS_host.empty()) {
        std::cerr << "Error: --host is required" << std::endl;
        gflags::ShowUsageWithFlags(argv[0]);
        return 1;
    }

    // Validate parameters
    if (FLAGS_put_ratio < 0.0 || FLAGS_put_ratio > 1.0) {
        std::cerr << "PUT ratio must be between 0.0 and 1.0" << std::endl;
        return 1;
    }

    if (!kv::IsValidValueSize(static_cast<uint32_t>(FLAGS_value_size))) {
        std::cerr << "Value size must be 4KB-64KB and 4KB aligned" << std::endl;
        return 1;
    }

    if (FLAGS_segments < 1 || FLAGS_segments > kv::MAX_SEGMENTS) {
        std::cerr << "Number of segments must be 1-" << kv::MAX_SEGMENTS << std::endl;
        return 1;
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    KVClient client;
    if (!client.Connect(FLAGS_host, static_cast<uint16_t>(FLAGS_port))) {
        return 1;
    }

    client.RunBenchmark(FLAGS_num_operations, static_cast<float>(FLAGS_put_ratio),
                        static_cast<uint32_t>(FLAGS_value_size), FLAGS_sgl, FLAGS_segments);

    std::cout << "Client finished" << std::endl;
    return 0;
}
