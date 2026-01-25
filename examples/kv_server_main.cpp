// RDMA KV Server main entry point

#include <gflags/gflags.h>
#include <signal.h>

#include <iostream>

#include "kv_server.h"

DEFINE_int32(port, 12345, "Server port to listen on");

int main(int argc, char* argv[]) {
    gflags::SetUsageMessage("RDMA KV Server - High-performance key-value server using RDMA");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    KVServer server;
    if (!server.Initialize(static_cast<uint16_t>(FLAGS_port))) {
        return 1;
    }

    server.Run();

    std::cout << "KV server stopped" << std::endl;
    return 0;
}
