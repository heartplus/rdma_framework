// RDMA KV Server implementation

#include "kv_server.h"

using namespace rdma;

volatile bool g_running = true;

void SignalHandler(int /*sig*/) {
    g_running = false;
}

bool KVServer::Initialize(uint16_t port) {
    // Initialize KV store (1GB storage)
    kv::StoreConfig store_config;
    store_config.total_storage_size = 1ULL * 1024 * 1024 * 1024;  // 1GB
    store_config.slot_size = kv::MAX_VALUE_SIZE;                   // 64KB slots

    if (!store_.Initialize(store_config)) {
        std::cerr << "Failed to initialize KV store" << std::endl;
        return false;
    }
    std::cout << "KV store initialized: " << store_.GetTotalSlots() << " slots, "
              << (store_.GetTotalBytes() / (1024 * 1024)) << " MB" << std::endl;

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

    // Create buffer pool with larger buffers to support 64KB values
    // Buffer size = header (16B) + max segments (16 * 8B = 128B) + max value (64KB)
    BufferPoolConfig bp_config;
    bp_config.buffer_size = kv::MAX_REQUEST_SIZE;  // ~65KB + overhead
    bp_config.buffer_count = 512;
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

    auto cm_result =
            ConnectionManager::Create(pd_.get(), send_cq_.get(), recv_cq_.get(), cm_config);
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
    dispatcher_.SetErrorHandler(
            this, [](void* ctx, uint16_t conn_id, ibv_wc_status status) {
                auto* self = static_cast<KVServer*>(ctx);
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
    std::cout << "KV server initialized on port " << port << std::endl;
    return true;
}

void KVServer::Run() {
    std::cout << "KV server running..." << std::endl;

    while (g_running) {
        // Accept new connections (non-blocking)
        AcceptConnections();

        // Use reactor to poll CQs (handles adaptive batching, backoff, etc.)
        reactor_->Poll();
    }

    PrintStats();
}

void KVServer::AcceptConnections() {
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

    // Create connection context
    auto* ctx = new ConnectionContext{conn, 0, 0, 0};
    conn_contexts_[conn->GetConnId()] = ctx;

    // Register with dispatcher
    dispatcher_.RegisterConnection(
            conn->GetConnId(), this, [](void* server_ctx, const ibv_wc& wc) {
                auto* self = static_cast<KVServer*>(server_ctx);
                self->HandleCompletion(wc);
            });

    // Post receive buffers
    PostRecvBuffers(conn, 32);

    std::cout << "Connection " << conn->GetConnId() << " established" << std::endl;
}

void KVServer::PostRecvBuffers(Connection* conn, int count) {
    for (int i = 0; i < count; ++i) {
        Buffer buf = buffer_pool_->Allocate();
        if (buf.IsValid()) {
            // Use buffer address as part of wr_id for tracking
            // We'll encode the buffer index in the slot field
            uint32_t buf_index = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(buf.GetAddr()) & 0xFFFFFFFF);
            uint64_t wr_id = WrId::Encode(conn->GetConnId(), RpcOpcode::RECV, 0, buf_index);

            // Track the buffer
            posted_recv_buffers_[wr_id] = {buf, conn->GetConnId()};

            conn->PostRecv(buf, wr_id);
        }
    }
}

void KVServer::HandleCompletion(const ibv_wc& wc) {
    if (wc.status != IBV_WC_SUCCESS) {
        return;
    }

    RpcOpcode opcode = WrId::GetOpcode(wc.wr_id);
    uint16_t conn_id = WrId::GetConnId(wc.wr_id);

    if (opcode == RpcOpcode::RECV) {
        // Handle received request
        HandleRequest(conn_id, wc);
    } else if (opcode == RpcOpcode::REQUEST_SEND) {
        // Send completed, free the buffer
        auto it = posted_send_buffers_.find(wc.wr_id);
        if (it != posted_send_buffers_.end()) {
            buffer_pool_->Free(it->second);
            posted_send_buffers_.erase(it);
        }
    }
}

void KVServer::HandleRequest(uint16_t conn_id, const ibv_wc& wc) {
    auto it = conn_contexts_.find(conn_id);
    if (it == conn_contexts_.end()) {
        return;
    }
    auto* ctx = it->second;

    // Find the posted receive buffer
    auto buf_it = posted_recv_buffers_.find(wc.wr_id);
    if (buf_it == posted_recv_buffers_.end()) {
        std::cerr << "Receive buffer not found for wr_id" << std::endl;
        return;
    }

    Buffer recv_buf = buf_it->second.buf;
    posted_recv_buffers_.erase(buf_it);

    // Validate request size
    if (wc.byte_len < sizeof(kv::RequestHeader)) {
        std::cerr << "Invalid request: too short (" << wc.byte_len << " bytes)" << std::endl;
        buffer_pool_->Free(recv_buf);
        PostRecvBuffers(ctx->conn, 1);
        return;
    }

    // Process the request
    ProcessRequest(ctx->conn, recv_buf.GetAddr(), wc.byte_len, ctx);

    // Free receive buffer and post new one
    buffer_pool_->Free(recv_buf);
    PostRecvBuffers(ctx->conn, 1);

    ++ctx->requests_processed;
}

void KVServer::ProcessRequest(Connection* conn, const void* request_data, size_t request_len,
                              ConnectionContext* ctx) {
    if (request_len < sizeof(kv::RequestHeader)) {
        SendErrorResponse(conn, 0, kv::Status::INVALID_REQUEST);
        return;
    }

    const auto* header = static_cast<const kv::RequestHeader*>(request_data);
    kv::OpType op = static_cast<kv::OpType>(header->op_type);

    if (op == kv::OpType::GET) {
        ProcessGet(conn, header, ctx);
    } else if (op == kv::OpType::PUT) {
        ProcessPut(conn, header, request_data, request_len, ctx);
    } else {
        SendErrorResponse(conn, header->key, kv::Status::INVALID_REQUEST);
    }
}

void KVServer::ProcessGet(Connection* conn, const kv::RequestHeader* header,
                          ConnectionContext* ctx) {
    Buffer response_buf = buffer_pool_->Allocate();
    if (!response_buf.IsValid()) {
        return;
    }

    auto* response = static_cast<kv::ResponseHeader*>(response_buf.GetAddr());
    uint8_t* value_ptr = response_buf.GetData() + sizeof(kv::ResponseHeader);

    uint32_t value_size = 0;
    kv::Status status =
            store_.Get(header->key, value_ptr, kv::MAX_VALUE_SIZE, &value_size);

    response->SetGetResponse(header->key, status, status == kv::Status::OK ? value_size : 0);

    size_t response_len = sizeof(kv::ResponseHeader);
    if (status == kv::Status::OK) {
        response_len += value_size;
    }

    uint64_t wr_id =
            WrId::Encode(conn->GetConnId(), RpcOpcode::REQUEST_SEND, 0,
                         static_cast<uint32_t>(ctx->gets_processed & 0xFFFFFFFF));

    // Track send buffer for freeing on completion
    posted_send_buffers_[wr_id] = response_buf;

    conn->Send(response_buf.Slice(0, response_len), wr_id);

    ++ctx->gets_processed;
}

void KVServer::ProcessPut(Connection* conn, const kv::RequestHeader* header,
                          const void* request_data, size_t request_len, ConnectionContext* ctx) {
    const uint8_t* data = static_cast<const uint8_t*>(request_data);
    size_t offset = sizeof(kv::RequestHeader);

    kv::Status status;

    if (header->segment_count == 0) {
        // Contiguous value
        if (request_len < offset + header->value_size) {
            status = kv::Status::INVALID_REQUEST;
        } else {
            status = store_.Put(header->key, data + offset, header->value_size);
        }
    } else {
        // Non-contiguous value with segment descriptors
        size_t desc_size = header->segment_count * sizeof(kv::SegmentDescriptor);
        if (request_len < offset + desc_size + header->value_size) {
            status = kv::Status::INVALID_REQUEST;
        } else {
            // Skip segment descriptors - the data follows them in order
            // Since the data is already contiguous in the receive buffer,
            // we can directly store it
            offset += desc_size;
            status = store_.Put(header->key, data + offset, header->value_size);
        }
    }

    // Send response
    Buffer response_buf = buffer_pool_->Allocate();
    if (!response_buf.IsValid()) {
        return;
    }

    auto* response = static_cast<kv::ResponseHeader*>(response_buf.GetAddr());
    response->SetPutResponse(header->key, status);

    uint64_t wr_id =
            WrId::Encode(conn->GetConnId(), RpcOpcode::REQUEST_SEND, 0,
                         static_cast<uint32_t>(ctx->puts_processed & 0xFFFFFFFF));

    // Track send buffer for freeing on completion
    posted_send_buffers_[wr_id] = response_buf;

    conn->Send(response_buf.Slice(0, sizeof(kv::ResponseHeader)), wr_id);

    ++ctx->puts_processed;
}

void KVServer::SendErrorResponse(Connection* conn, uint64_t key, kv::Status status) {
    Buffer response_buf = buffer_pool_->Allocate();
    if (!response_buf.IsValid()) {
        return;
    }

    auto* response = static_cast<kv::ResponseHeader*>(response_buf.GetAddr());
    response->SetPutResponse(key, status);

    uint64_t wr_id = WrId::Encode(conn->GetConnId(), RpcOpcode::REQUEST_SEND, 0, 0);

    // Track send buffer for freeing on completion
    posted_send_buffers_[wr_id] = response_buf;

    conn->Send(response_buf.Slice(0, sizeof(kv::ResponseHeader)), wr_id);
}


void KVServer::OnError(uint16_t conn_id, ibv_wc_status status) {
    std::cerr << "Error on connection " << conn_id << ": " << ibv_wc_status_str(status)
              << std::endl;

    // Clean up connection context
    auto it = conn_contexts_.find(conn_id);
    if (it != conn_contexts_.end()) {
        delete it->second;
        conn_contexts_.erase(it);
    }

    conn_manager_->RemoveConnection(conn_id);
}

void KVServer::PrintStats() {
    std::cout << "\n=== KV Server Statistics ===" << std::endl;
    std::cout << "Store usage: " << store_.GetUsedSlots() << "/" << store_.GetTotalSlots()
              << " slots (" << (store_.GetUsedBytes() / 1024) << " KB / "
              << (store_.GetTotalBytes() / (1024 * 1024)) << " MB)" << std::endl;
    std::cout << "Load factor: " << (store_.GetLoadFactor() * 100) << "%" << std::endl;

    uint64_t total_requests = 0;
    uint64_t total_gets = 0;
    uint64_t total_puts = 0;
    for (const auto& pair : conn_contexts_) {
        total_requests += pair.second->requests_processed;
        total_gets += pair.second->gets_processed;
        total_puts += pair.second->puts_processed;
    }
    std::cout << "Total requests: " << total_requests << " (GET: " << total_gets
              << ", PUT: " << total_puts << ")" << std::endl;
}
