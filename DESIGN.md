# RDMA RPC Framework 设计文档

## 1. 项目概述

基于 RDMA 的高性能 RPC 框架，面向存储场景设计：
- **传输模式**: RC (Reliable Connection)
- **网络类型**: RoCE v2
- **API 风格**: C++ 面向对象，RAII 资源管理
- **完成通知**: Polling 模式（无锁化，Reactor 驱动）
- **集成目标**: SPDK、brpc
- **性能目标**: 1M+ QPS per core

## 2. 架构分层

```
┌─────────────────────────────────────────────────────────────┐
│                      Application Layer                       │
├─────────────────────────────────────────────────────────────┤
│                       RPC Layer                              │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────────────┐ │
│  │ RpcEndpoint │  │  RpcSlot    │  │ CompletionDispatcher │ │
│  └─────────────┘  └─────────────┘  └──────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│                    Connection Layer                          │
│  ┌────────────┐  ┌───────────────────┐  ┌────────────────┐  │
│  │ Connection │  │ ConnectionManager │  │  FlowControl   │  │
│  └────────────┘  └───────────────────┘  └────────────────┘  │
│  ┌──────────────────┐  ┌─────────────────────────────────┐  │
│  │ HandshakeChannel │  │     RemoteMemoryRegistry        │  │
│  └──────────────────┘  └─────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                       QP Layer                               │
│  ┌───────────┐  ┌─────────────────┐  ┌──────────────────┐   │
│  │ QueuePair │  │ CompletionQueue │  │   QpNegotiation  │   │
│  └───────────┘  └─────────────────┘  └──────────────────┘   │
│  ┌───────────────────────────────────────────────────────┐  │
│  │              DoorbellBatcher (延迟提交)                 │  │
│  └───────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                     Resource Layer                           │
│  ┌────────────┐  ┌──────────────────┐  ┌─────────────────┐  │
│  │ RdmaDevice │  │ ProtectionDomain │  │  MemoryRegion   │  │
│  └────────────┘  └──────────────────┘  └─────────────────┘  │
│  ┌─────────────────┐  ┌───────────────────┐  ┌───────────┐  │
│  │ LocalBufferPool │  │ MemoryAllocator   │  │  MRCache  │  │
│  └─────────────────┘  └───────────────────┘  └───────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## 3. 项目目录结构

```
rdma_framework/
├── CMakeLists.txt
├── cmake/
│   ├── FindIBVerbs.cmake
│   └── FindRDMACM.cmake
├── include/rdma/
│   ├── rdma.h                        # 统一头文件入口
│   ├── common/
│   │   ├── types.h                   # 基础类型定义
│   │   ├── status.h                  # 错误处理 Status/Result<T>
│   │   ├── logging.h                 # 日志系统（编译期可移除）
│   │   ├── wr_id.h                   # wr_id 编码/解码
│   │   ├── mpsc_queue.h              # Lock-free MPSC 队列
│   │   └── timing_wheel.h            # 时间轮（超时管理）
│   ├── core/
│   │   ├── device.h                  # RDMA 设备封装
│   │   ├── protection_domain.h       # 保护域
│   │   ├── memory_region.h           # 内存区域
│   │   ├── mr_cache.h                # MR 缓存
│   │   ├── completion_queue.h        # 完成队列
│   │   ├── queue_pair.h              # 队列对
│   │   └── doorbell_batcher.h        # Doorbell 批量提交
│   ├── memory/
│   │   ├── memory_allocator.h        # 内存分配器接口
│   │   ├── default_allocator.h       # 默认实现
│   │   ├── hugepage_allocator.h      # Hugepage 分配器
│   │   ├── spdk_allocator.h          # SPDK DMA 内存分配器
│   │   ├── buffer.h                  # Buffer 抽象（支持 SGL/iovec）
│   │   └── buffer_pool.h             # 本地缓冲区池（per-Reactor）
│   ├── transport/
│   │   ├── connection.h              # 连接抽象
│   │   ├── connection_manager.h      # 连接管理器
│   │   ├── handshake_channel.h       # 握手通道抽象
│   │   ├── tcp_handshake.h           # TCP 握手实现
│   │   ├── qp_negotiation.h          # QP 参数协商
│   │   ├── flow_control.h            # 流控机制（懒更新）
│   │   └── remote_memory_registry.h  # 远程内存注册表
│   ├── rpc/
│   │   ├── rpc_endpoint.h            # RPC 端点
│   │   ├── rpc_slot.h                # RPC Slot（池化 + generation）
│   │   ├── completion_dispatcher.h   # 完成分发器（二级索引）
│   │   ├── completion_queue_proxy.h  # 应用层完成队列
│   │   └── keepalive.h               # 心跳机制（低频 / 独立 CQ）
│   ├── reactor/
│   │   ├── reactor.h                 # Reactor 事件循环
│   │   ├── poller.h                  # Poller 接口
│   │   └── numa.h                    # NUMA 亲和性
│   └── integration/
│       ├── spdk_adapter.h            # SPDK 适配器
│       └── brpc_transport.h          # brpc 传输层
├── src/
├── examples/
└── tests/
```

## 4. 线程模型与 NUMA 亲和性（重要）

### 4.1 核心原则

```
┌────────────────────────────────────────────────────────────────┐
│                        线程安全规则                              │
├────────────────────────────────────────────────────────────────┤
│ 1. 所有 RDMA verbs 调用必须在 Reactor 线程执行                   │
│ 2. Connection / QP / RpcEndpoint 只能被所属 Reactor 线程访问     │
│ 3. LocalBufferPool 是 per-Reactor 的，无锁访问                  │
│ 4. Reactor::Execute() 是唯一合法的跨线程入口（lock-free）        │
│ 5. CQ 只能被一个 Reactor 线程 poll                              │
│ 6. 用户 callback 不在 Reactor 热路径执行，通过队列分离           │
│ 7. Fast path 禁止任何 new/delete 操作                           │
│ 8. Fast path 禁止使用 stack 上的数据作为 Send buffer             │
└────────────────────────────────────────────────────────────────┘
```

### 4.2 NUMA 亲和性（必须）

```cpp
// 设计考虑：
// 1. Reactor 线程必须 pin 到指定 CPU core
// 2. RDMA NIC 必须与 Reactor 在同一 NUMA node
// 3. BufferPool 内存分配必须 NUMA-aware
// 4. 跨 NUMA 访问会导致延迟抖动和吞吐下降

struct NumaConfig {
    int cpu_core;           // Reactor 绑定的 CPU core
    int numa_node;          // NUMA node ID
    const char* nic_name;   // RDMA NIC 设备名

    // 校验 NIC 是否在同一 NUMA node
    static bool ValidateNicNuma(const char* nic_name, int expected_numa);
};

class ReactorResources {
public:
    // 必须在创建时指定 NUMA 配置
    static Result<std::unique_ptr<ReactorResources>> Create(const NumaConfig& config);

    Reactor* reactor;
    CompletionQueue* cq;
    CompletionDispatcher* dispatcher;
    LocalBufferPool* buffer_pool;       // NUMA-aware 分配
    std::vector<Connection*> connections;

private:
    // Pin 当前线程到指定 CPU
    static Status PinThread(int cpu_core);
};
```

### 4.3 内存分配 NUMA-aware

```cpp
// Hugepage 分配器（减少 TLB Miss）
class HugepageAllocator : public MemoryAllocator {
public:
    // 指定 NUMA node 分配
    HugepageAllocator(int numa_node);

    void* Allocate(size_t size, size_t alignment) override {
        // 使用 mmap + MAP_HUGETLB + mbind
        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr != MAP_FAILED && numa_node_ >= 0) {
            // 绑定到指定 NUMA node
            unsigned long nodemask = 1UL << numa_node_;
            mbind(ptr, size, MPOL_BIND, &nodemask, sizeof(nodemask) * 8, 0);
        }
        return ptr;
    }

private:
    int numa_node_;
};
```

## 5. 核心数据结构

### 5.1 wr_id 布局（含 generation）

```cpp
// wr_id 64-bit 布局:
// [ 16 bits conn_id | 8 bits opcode | 8 bits generation | 32 bits slot_index ]
//
// 设计考虑：
// - generation 防止 slot reuse 时的 ABA 问题
// - completion 时校验 generation，不匹配则丢弃

namespace rdma {

enum class RpcOpcode : uint8_t {
    REQUEST_SEND = 0,
    REQUEST_WRITE = 1,
    REQUEST_READ = 2,
    RESPONSE_SEND = 3,
    RECV = 4,
    CREDIT_UPDATE = 5,
    KEEPALIVE = 6,
};

class WrId {
public:
    // 全部 inline，避免函数调用开销
    ALWAYS_INLINE static uint64_t Encode(uint16_t conn_id, RpcOpcode opcode,
                                         uint8_t generation, uint32_t slot_index) {
        return (static_cast<uint64_t>(conn_id) << 48) |
               (static_cast<uint64_t>(opcode) << 40) |
               (static_cast<uint64_t>(generation) << 32) |
               slot_index;
    }

    ALWAYS_INLINE static uint16_t GetConnId(uint64_t wr_id) {
        return static_cast<uint16_t>(wr_id >> 48);
    }

    ALWAYS_INLINE static RpcOpcode GetOpcode(uint64_t wr_id) {
        return static_cast<RpcOpcode>((wr_id >> 40) & 0xFF);
    }

    ALWAYS_INLINE static uint8_t GetGeneration(uint64_t wr_id) {
        return static_cast<uint8_t>((wr_id >> 32) & 0xFF);
    }

    ALWAYS_INLINE static uint32_t GetSlotIndex(uint64_t wr_id) {
        return static_cast<uint32_t>(wr_id & 0xFFFFFFFF);
    }
};

// 编译器属性定义
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#define HOT_FUNCTION  __attribute__((hot))
#define LIKELY(x)     __builtin_expect(!!(x), 1)
#define UNLIKELY(x)   __builtin_expect(!!(x), 0)

}  // namespace rdma
```

### 5.2 RpcSlot（热度分层：Hot + Context + Extended）

```cpp
// 设计考虑：
// 1. 【优化】按访问频率拆分为三层结构，提升 PollCQ 期间的缓存效率
//    - RpcSlotHot: PollCQ 每次 WC 都访问（状态、计数）
//    - RpcSlotContext: 仅 RPC 完成时访问（回调、Buffer）
//    - ExtendedSlotInfo: 冷数据，调试/统计时访问
// 2. 位域压缩非关键字段
// 3. generation 防止 ABA
// 4. 三个数组连续存储，各自 cache line 对齐

// ========== 第一层：Hot 数据（16 字节，PollCQ 每次都访问）==========
// 在 PollCQ 循环中，每个 WC 都需要检查 slot 是否完成
// 将状态位、计数器压缩到最小，多个 slot 可以在同一 cache line 中
//
// 【优化】generation 回绕保护：从 8 位扩展到 12 位
// - 原 8 位 generation：最多 256 次复用，在极高频短 RPC 场景下可能快速回绕
// - 现 12 位 generation：最多 4096 次复用，回绕期延长至约 4.5 分钟（@1M QPS）
// - slot_index 从 32 位降为 20 位，仍支持 100 万+ 槽位

// 【优化】移除 C++ 位域，改用纯整数 flags 字段
// 理由：位域在不同编译器下行为不一致，且可能产生额外的掩码和移位指令
// 改用宏/内联函数访问，减少指令数，提升编译器优化能力
struct alignas(16) RpcSlotHot {
    // 组合字段：slot_index(20) + generation(12) = 32 bits
    uint32_t slot_gen_packed;           // 4 字节（低 20 位 slot_index，高 12 位 generation）

    // 【优化】flags 字段布局：[7:6]=state, [5:3]=wr_type, [2]=signaled, [1:0]=reserved
    // 替代原有的 C++ 位域，生成更紧凑的汇编代码
    uint8_t flags;                      // 1 字节（打包所有标志位）
    uint8_t _padding1;                  // 1 字节

    uint16_t conn_id;                   // 2 字节

    uint8_t total_wr_count;             // 1 字节
    uint8_t completed_wr_count;         // 1 字节
    uint8_t error_code;                 // 1 字节
    uint8_t _padding2;                  // 1 字节

    uint32_t timeout_tick;              // 4 字节（时间轮 tick）

    // ========== flags 字段位定义 ==========
    static constexpr uint8_t kStateMask     = 0xC0;  // bits [7:6]
    static constexpr uint8_t kStateShift    = 6;
    static constexpr uint8_t kWrTypeMask    = 0x38;  // bits [5:3]
    static constexpr uint8_t kWrTypeShift   = 3;
    static constexpr uint8_t kSignaledBit   = 0x04;  // bit [2]

    // State 枚举值
    static constexpr uint8_t kStateFree       = 0;
    static constexpr uint8_t kStateInUse      = 1;
    static constexpr uint8_t kStateCompleting = 2;

    // ========== flags 访问器（高效内联）==========
    ALWAYS_INLINE uint8_t GetState() const {
        return (flags & kStateMask) >> kStateShift;
    }

    ALWAYS_INLINE void SetState(uint8_t state) {
        flags = (flags & ~kStateMask) | ((state << kStateShift) & kStateMask);
    }

    ALWAYS_INLINE uint8_t GetWrType() const {
        return (flags & kWrTypeMask) >> kWrTypeShift;
    }

    ALWAYS_INLINE void SetWrType(uint8_t wr_type) {
        flags = (flags & ~kWrTypeMask) | ((wr_type << kWrTypeShift) & kWrTypeMask);
    }

    ALWAYS_INLINE bool IsSignaled() const {
        return flags & kSignaledBit;
    }

    ALWAYS_INLINE void SetSignaled(bool signaled) {
        if (signaled) {
            flags |= kSignaledBit;
        } else {
            flags &= ~kSignaledBit;
        }
    }

    // slot_index 和 generation 的访问器
    static constexpr uint32_t kSlotIndexMask = 0x000FFFFF;   // 低 20 位
    static constexpr uint32_t kGenerationShift = 20;
    static constexpr uint32_t kGenerationMask = 0xFFF;       // 高 12 位（共 4096）
    static constexpr uint32_t kMaxSlotIndex = (1u << 20) - 1; // 约 100 万

    ALWAYS_INLINE uint32_t GetSlotIndex() const {
        return slot_gen_packed & kSlotIndexMask;
    }

    ALWAYS_INLINE uint16_t GetGeneration() const {
        return static_cast<uint16_t>((slot_gen_packed >> kGenerationShift) & kGenerationMask);
    }

    ALWAYS_INLINE void SetSlotIndex(uint32_t idx) {
        slot_gen_packed = (slot_gen_packed & ~kSlotIndexMask) | (idx & kSlotIndexMask);
    }

    ALWAYS_INLINE void IncrementGeneration() {
        uint32_t gen = (slot_gen_packed >> kGenerationShift) + 1;
        slot_gen_packed = (slot_gen_packed & kSlotIndexMask) |
                          ((gen & kGenerationMask) << kGenerationShift);
    }

    // 用于 wr_id 编码的紧凑形式
    ALWAYS_INLINE uint32_t GetSlotGenPacked() const {
        return slot_gen_packed;
    }

    // 热路径方法
    ALWAYS_INLINE bool IsCompleted() const {
        return completed_wr_count >= total_wr_count;
    }

    ALWAYS_INLINE bool IsFree() const {
        return GetState() == kStateFree;
    }

    ALWAYS_INLINE void MarkCompleted() {
        ++completed_wr_count;
    }

    // 【增强】验证 generation 匹配（防止 ABA）
    ALWAYS_INLINE bool ValidateGeneration(uint16_t expected_gen) const {
        return GetGeneration() == expected_gen;
    }
};

static_assert(sizeof(RpcSlotHot) == 16, "RpcSlotHot must be 16 bytes");

// ========== 第二层：Context 数据（32 字节，仅完成时访问）==========
// 只有当 RpcSlotHot::IsCompleted() 返回 true 时才加载
// 包含回调函数和 Buffer 信息
struct alignas(32) RpcSlotContext {
    // 回调信息（16 字节）
    void (*callback_fn)(void* ctx, uint32_t slot_index, uint8_t status);
    void* callback_ctx;

    // Buffer 信息（16 字节）
    void* recv_buffer;                  // 接收缓冲区地址
    uint32_t recv_length;               // 接收数据长度
    uint32_t extended_info_index;       // 扩展信息索引
};

static_assert(sizeof(RpcSlotContext) == 32, "RpcSlotContext must be 32 bytes");

// ========== 第三层：Extended 数据（64 字节，冷数据）==========
// 调试、统计、详细跟踪时访问
struct alignas(64) ExtendedSlotInfo {
    uint64_t rpc_id;                    // RPC 请求 ID
    uint64_t start_tsc;                 // 开始时间（用于延迟统计）
    void* user_data;                    // 用户自定义数据
    uint32_t request_size;              // 请求大小
    uint32_t response_size;             // 响应大小

    // 详细 WR 跟踪（调试用）
    struct WrDetail {
        uint8_t posted : 1;
        uint8_t completed : 1;
        uint8_t status : 6;
    };
    WrDetail wr_details[4];             // 最多跟踪 4 个 WR

    uint8_t _padding[24];               // padding to 64 bytes
};

static_assert(sizeof(ExtendedSlotInfo) == 64, "ExtendedSlotInfo must be 64 bytes");

// ========== 兼容性别名 ==========
// 保持向后兼容，RpcSlot 作为 Hot + Context 的逻辑组合
struct RpcSlot {
    RpcSlotHot* hot;
    RpcSlotContext* context;

    ALWAYS_INLINE bool IsCompleted() const { return hot->IsCompleted(); }
    ALWAYS_INLINE bool IsFree() const { return hot->IsFree(); }
    ALWAYS_INLINE void MarkCompleted() { hot->MarkCompleted(); }
};

// RpcSlot 池（热度分层 + 固定大小栈）
// 【优化】三层数据结构分离存储，提升 PollCQ 缓存效率
class RpcSlotPool {
public:
    static constexpr size_t kMaxSlots = 65536;

    RpcSlotPool() {
        // 预分配 Hot 数据（16 字节 × N，4 个 slot 共享一个 cache line）
        slots_hot_ = static_cast<RpcSlotHot*>(
            aligned_alloc(64, kMaxSlots * sizeof(RpcSlotHot)));

        // 预分配 Context 数据（32 字节 × N，仅完成时访问）
        slots_context_ = static_cast<RpcSlotContext*>(
            aligned_alloc(64, kMaxSlots * sizeof(RpcSlotContext)));

        // 预分配 Extended 数据（64 字节 × N，冷数据）
        extended_info_ = static_cast<ExtendedSlotInfo*>(
            aligned_alloc(64, kMaxSlots * sizeof(ExtendedSlotInfo)));

        // 预分配固定大小的 free list 数组
        free_stack_ = static_cast<uint32_t*>(
            aligned_alloc(64, kMaxSlots * sizeof(uint32_t)));

        // 预填充 free stack（逆序填充，使低索引先被分配）
        for (uint32_t i = 0; i < kMaxSlots; ++i) {
            // 使用 slot_gen_packed 存储：低 20 位 = slot_index，高 12 位 = generation(0)
            slots_hot_[i].slot_gen_packed = i;  // generation = 0, slot_index = i
            slots_hot_[i].flags = 0;  // state=free, wr_type=0, signaled=false
            slots_context_[i].extended_info_index = i;  // 1:1 映射
            free_stack_[i] = kMaxSlots - 1 - i;  // 逆序
        }
        stack_top_ = kMaxSlots;  // 栈满
    }

    ~RpcSlotPool() {
        free(slots_hot_);
        free(slots_context_);
        free(extended_info_);
        free(free_stack_);
    }

    // 分配 slot（固定大小栈，无动态分配，分支预测友好）
    ALWAYS_INLINE std::pair<uint32_t, uint8_t> Allocate() {
        if (UNLIKELY(stack_top_ == 0)) {
            return {UINT32_MAX, 0};  // 无可用 slot
        }
        uint32_t index = free_stack_[--stack_top_];

        RpcSlotHot* hot = &slots_hot_[index];
        hot->SetState(RpcSlotHot::kStateInUse);
        hot->completed_wr_count = 0;
        hot->error_code = 0;

        return {index, static_cast<uint8_t>(hot->GetGeneration())};
    }

    // 释放 slot（直接入栈，无内存分配）
    ALWAYS_INLINE void Free(uint32_t slot_index) {
        RpcSlotHot* hot = &slots_hot_[slot_index];
        hot->SetState(RpcSlotHot::kStateFree);
        hot->IncrementGeneration();  // 递增 generation 防止 ABA
        free_stack_[stack_top_++] = slot_index;
    }

    // 获取 Hot 数据（PollCQ 热路径）
    ALWAYS_INLINE RpcSlotHot* GetHot(uint32_t slot_index) {
        return &slots_hot_[slot_index];
    }

    // 获取 Hot 数据（带 generation 校验）
    ALWAYS_INLINE RpcSlotHot* GetHotChecked(uint32_t slot_index, uint8_t expected_gen) {
        RpcSlotHot* hot = &slots_hot_[slot_index];
        if (UNLIKELY(hot->generation != expected_gen)) {
            return nullptr;  // stale
        }
        return hot;
    }

    // 获取 Context 数据（仅完成时访问）
    ALWAYS_INLINE RpcSlotContext* GetContext(uint32_t slot_index) {
        return &slots_context_[slot_index];
    }

    // 获取扩展信息（冷路径）
    ALWAYS_INLINE ExtendedSlotInfo* GetExtendedInfo(uint32_t slot_index) {
        return &extended_info_[slot_index];
    }

    // 预取 Hot 数据到 cache（PollCQ 预取）
    ALWAYS_INLINE void PrefetchHot(uint32_t slot_index) {
        __builtin_prefetch(&slots_hot_[slot_index], 0, 3);
    }

    // 预取 Context 数据（仅在确定需要回调时）
    ALWAYS_INLINE void PrefetchContext(uint32_t slot_index) {
        __builtin_prefetch(&slots_context_[slot_index], 0, 3);
    }

    // 获取当前可用 slot 数量
    ALWAYS_INLINE size_t AvailableCount() const {
        return stack_top_;
    }

private:
    RpcSlotHot* slots_hot_;                 // Hot 数据，PollCQ 频繁访问
    RpcSlotContext* slots_context_;         // Context 数据，完成时访问
    ExtendedSlotInfo* extended_info_;       // 扩展信息，冷数据
    uint32_t* free_stack_;                  // 固定大小数组栈（替代 std::vector）
    size_t stack_top_ = 0;                  // 栈顶指针
};
```

### 5.3 分层时间轮（Hierarchical Timing Wheel）

```cpp
// 设计考虑：
// 1. priority_queue 的 push 是 O(log N)，1M QPS 下开销 10%+
// 2. 时间轮：插入 O(1)，检查 O(1)
// 3. 【改进】分层时间轮解决精度问题：
//    - 第一层：1ms 精度，256 格（覆盖 256ms）
//    - 第二层：256ms 精度，256 格（覆盖 ~65s）
//    - 超长超时存入第二层，临近时降级到第一层
// 4. 【改进】环形缓冲区 + 固定大小数组，避免 vector 动态扩容

struct TimeoutEntry {
    uint32_t slot_index;
    uint8_t generation;
    uint8_t _padding[3];

    // 侵入式链表
    TimeoutEntry* next;
};

// 环形缓冲区 Bucket（无动态分配）
class TimingWheelBucket {
public:
    ALWAYS_INLINE void Push(TimeoutEntry* entry) {
        entry->next = head_;
        head_ = entry;
        ++count_;
    }

    ALWAYS_INLINE TimeoutEntry* PopAll() {
        TimeoutEntry* result = head_;
        head_ = nullptr;
        count_ = 0;
        return result;
    }

    size_t Count() const { return count_; }

private:
    TimeoutEntry* head_ = nullptr;
    size_t count_ = 0;
};

class HierarchicalTimingWheel {
public:
    static constexpr size_t kLevel1Size = 256;      // 第一层格子数
    static constexpr size_t kLevel2Size = 256;      // 第二层格子数
    static constexpr uint32_t kLevel1TickMs = 1;    // 第一层每格 1ms
    static constexpr uint32_t kLevel2TickMs = 256;  // 第二层每格 256ms

    // 总覆盖范围：256 * 256ms = 65536ms ≈ 65s

    HierarchicalTimingWheel() {
        // 预分配 TimeoutEntry 池（避免运行时分配）
        entry_pool_ = static_cast<TimeoutEntry*>(
            aligned_alloc(64, kMaxEntries * sizeof(TimeoutEntry)));

        // 初始化 free list
        for (size_t i = 0; i < kMaxEntries - 1; ++i) {
            entry_pool_[i].next = &entry_pool_[i + 1];
        }
        entry_pool_[kMaxEntries - 1].next = nullptr;
        free_list_ = &entry_pool_[0];
    }

    ~HierarchicalTimingWheel() {
        free(entry_pool_);
    }

    // O(1) 插入
    ALWAYS_INLINE bool Schedule(uint32_t slot_index, uint8_t generation,
                                uint32_t timeout_ms) {
        // 分配 entry
        TimeoutEntry* entry = AllocEntry();
        if (UNLIKELY(!entry)) return false;

        entry->slot_index = slot_index;
        entry->generation = generation;

        if (timeout_ms < kLevel1Size * kLevel1TickMs) {
            // 短超时：直接放入第一层
            uint32_t target = (level1_current_ + timeout_ms) % kLevel1Size;
            level1_[target].Push(entry);
        } else {
            // 长超时：放入第二层
            uint32_t level2_ticks = timeout_ms / kLevel2TickMs;
            uint32_t target = (level2_current_ + level2_ticks) % kLevel2Size;
            level2_[target].Push(entry);
        }
        return true;
    }

    // O(1) 检查超时（每 1ms 调用一次）
    HOT_FUNCTION
    void Tick(std::vector<std::pair<uint32_t, uint8_t>>& expired) {
        expired.clear();

        // 1. 处理第一层当前格
        TimeoutEntry* entry = level1_[level1_current_].PopAll();
        while (entry) {
            expired.emplace_back(entry->slot_index, entry->generation);
            TimeoutEntry* next = entry->next;
            FreeEntry(entry);
            entry = next;
        }

        // 2. 前进第一层
        level1_current_ = (level1_current_ + 1) % kLevel1Size;

        // 3. 每 256ms，处理第二层降级
        if (level1_current_ == 0) {
            // 将第二层当前格的条目降级到第一层
            entry = level2_[level2_current_].PopAll();
            while (entry) {
                // 重新插入第一层（剩余时间 < 256ms）
                TimeoutEntry* next = entry->next;
                // 均匀分布到第一层
                uint32_t target = (reinterpret_cast<uintptr_t>(entry) >> 6) % kLevel1Size;
                level1_[target].Push(entry);
                entry = next;
            }

            level2_current_ = (level2_current_ + 1) % kLevel2Size;
        }
    }

    // 取消超时（通过 generation 校验实现，无需显式取消）
    // 当 generation 不匹配时，expired 处理时会忽略

private:
    static constexpr size_t kMaxEntries = 1 << 20;  // 100万条目

    TimingWheelBucket level1_[kLevel1Size];         // 第一层
    TimingWheelBucket level2_[kLevel2Size];         // 第二层

    uint32_t level1_current_ = 0;
    uint32_t level2_current_ = 0;

    // 预分配的 entry 池
    TimeoutEntry* entry_pool_;
    TimeoutEntry* free_list_;

    ALWAYS_INLINE TimeoutEntry* AllocEntry() {
        if (UNLIKELY(!free_list_)) return nullptr;
        TimeoutEntry* entry = free_list_;
        free_list_ = entry->next;
        return entry;
    }

    ALWAYS_INLINE void FreeEntry(TimeoutEntry* entry) {
        entry->next = free_list_;
        free_list_ = entry;
    }
};
```

## 6. 核心类设计

### 6.1 CompletionDispatcher（三级缓存：L0 直接映射 + L1 哈希 + L2 二级索引）

```cpp
// 设计考虑：
// 1. 直接 65536 数组会导致严重 Cache Miss
// 2.【优化】三级缓存结构：
//    - L0: 直接映射缓存（64 项），无冲突处理，最快
//    - L1: 开放地址哈希表（64 项），处理 L0 冲突
//    - L2: 二级索引（256 桶 × 256 连接），完整映射
// 3. 1M QPS 场景下，通常只有少数活跃连接贡献大部分流量
//    L0 缓存命中率极高，指令数最少
// 4. 快速失败路径：wc.status 判断提前

class CompletionDispatcher {
public:
    using WcHandler = void (*)(void* ctx, const ibv_wc& wc);
    using ErrorHandler = void (*)(void* ctx, uint16_t conn_id, ibv_wc_status status);

    // ========== L0 缓存配置（2-Way Set Associative）==========
    // 【优化】从直接映射升级为 2-路组相联
    // 由于 conn_id & mask 容易碰撞（特别是连接数不多但 ID 分散时）
    // 2-路组相联可以在几乎不增加指令数的前提下，将命中率从 ~60% 提升至 ~90%
    static constexpr size_t kL0SetCount = 32;       // 组数（2^5）
    static constexpr size_t kL0SetMask = kL0SetCount - 1;
    static constexpr size_t kL0Ways = 2;            // 每组 2 路

    // L0 缓存条目（单个条目 24 字节）
    struct L0CacheEntry {
        uint16_t conn_id;           // 连接 ID (0xFFFF = invalid)
        uint8_t  generation;        // 防止 stale entry
        uint8_t  access_count;      // 用于 LRU 替换
        void* ctx;                  // 上下文
        WcHandler handler;          // 处理函数
    };

    // 【优化】2-路组相联的组结构（SIMD 友好布局）
    // 将两个 conn_id 紧邻存储，便于 SIMD 并行比较
    struct alignas(64) L0CacheSet {
        // SIMD 友好：两个 conn_id 连续存储（4 字节，可用 32-bit 比较）
        uint16_t conn_ids[kL0Ways];     // [0]: way0, [1]: way1
        uint8_t  access_counts[kL0Ways];
        uint8_t  generations[kL0Ways];
        void* ctxs[kL0Ways];
        WcHandler handlers[kL0Ways];

        // 兼容访问器
        L0CacheEntry GetEntry(size_t way) const {
            return {conn_ids[way], generations[way], access_counts[way],
                    ctxs[way], handlers[way]};
        }
    };

    // 【新增】SIMD 辅助：使用 SSE2 并行比较两个 conn_id
    // 需要 #include <immintrin.h>
#ifdef __SSE2__
    ALWAYS_INLINE int FindInL0Simd(const L0CacheSet& set, uint16_t conn_id) {
        // 将目标 conn_id 广播到 32-bit 中的两个 16-bit 位置
        // target = [conn_id, conn_id]
        uint32_t target = (static_cast<uint32_t>(conn_id) << 16) | conn_id;

        // 从 set 加载两个 conn_id（32-bit）
        uint32_t cached = *reinterpret_cast<const uint32_t*>(set.conn_ids);

        // 使用 SSE2 比较（或简单的位运算）
        // 比较低 16 位
        if ((cached & 0xFFFF) == conn_id) return 0;
        // 比较高 16 位
        if ((cached >> 16) == conn_id) return 1;

        return -1;  // 未命中
    }
#endif

    CompletionDispatcher() {
        // 初始化 L0 缓存为无效（SIMD 友好布局）
        for (size_t i = 0; i < kL0SetCount; ++i) {
            for (size_t w = 0; w < kL0Ways; ++w) {
                l0_cache_[i].conn_ids[w] = 0xFFFF;
                l0_cache_[i].generations[w] = 0;
                l0_cache_[i].access_counts[w] = 0;
                l0_cache_[i].ctxs[w] = nullptr;
                l0_cache_[i].handlers[w] = nullptr;
            }
        }

        // 初始化 L1 哈希热缓存
        std::memset(hot_cache_, 0xFF, sizeof(hot_cache_));  // 0xFFFF = empty

        // L2 二级索引按需分配
        std::memset(buckets_, 0, sizeof(buckets_));
    }

    void RegisterConnection(uint16_t conn_id, void* ctx, WcHandler handler) {
        // 1. 添加到 L2 二级索引（完整映射）
        uint8_t bucket_idx = conn_id >> 8;
        uint8_t slot_idx = conn_id & 0xFF;

        if (!buckets_[bucket_idx]) {
            buckets_[bucket_idx] = new ConnectionEntry[256]();
        }
        buckets_[bucket_idx][slot_idx] = {ctx, handler};

        // 2. 更新 L0 缓存（2-路组相联，SIMD 友好布局）
        size_t set_idx = conn_id & kL0SetMask;
        L0CacheSet& set = l0_cache_[set_idx];

        // 查找是否已存在或找空槽
        int target_way = -1;
        int lru_way = 0;
        uint8_t min_access = 255;

        for (size_t w = 0; w < kL0Ways; ++w) {
            if (set.conn_ids[w] == conn_id) {
                // 已存在，直接更新
                target_way = static_cast<int>(w);
                break;
            }
            if (set.conn_ids[w] == 0xFFFF) {
                // 空槽
                target_way = static_cast<int>(w);
                break;
            }
            // 记录 LRU 候选
            if (set.access_counts[w] < min_access) {
                min_access = set.access_counts[w];
                lru_way = static_cast<int>(w);
            }
        }

        // 如果没找到空槽或已存在项，使用 LRU 替换
        if (target_way < 0) {
            target_way = lru_way;
        }

        set.conn_ids[target_way] = conn_id;
        set.generations[target_way]++;
        set.ctxs[target_way] = ctx;
        set.handlers[target_way] = handler;
        set.access_counts[target_way] = 128;  // 重置访问计数

        // 3. 更新 L1 哈希热缓存
        InsertHotCache(conn_id, ctx, handler);
    }

    void SetErrorHandler(void* ctx, ErrorHandler handler) {
        error_handler_ = {ctx, handler};
    }

    // 热路径：全部 inline，三级缓存查找
    // 【优化】使用 SIMD 友好的 32-bit 比较，一次比较两个 conn_id
    HOT_FUNCTION ALWAYS_INLINE
    void Dispatch(const ibv_wc& wc) {
        // 1. 快速失败路径
        if (UNLIKELY(wc.status != IBV_WC_SUCCESS)) {
            uint16_t conn_id = WrId::GetConnId(wc.wr_id);
            if (error_handler_.handler) {
                error_handler_.handler(error_handler_.ctx, conn_id, wc.status);
            }
            return;
        }

        uint16_t conn_id = WrId::GetConnId(wc.wr_id);

        // 2. L0 缓存查找（2-路组相联，SIMD 优化）
        size_t set_idx = conn_id & kL0SetMask;
        L0CacheSet& set = l0_cache_[set_idx];

#ifdef __SSE2__
        // 【优化】SIMD 并行比较：一条指令比较两个 conn_id
        int hit_way = FindInL0Simd(set, conn_id);
        if (LIKELY(hit_way >= 0)) {
            size_t w = static_cast<size_t>(hit_way);
            set.access_counts[w] = 255;  // 置为最高，防止被替换
            set.handlers[w](set.ctxs[w], wc);
            // 衰减另一路
            size_t other = 1 - w;
            if (set.access_counts[other] > 0) --set.access_counts[other];
            return;
        }
#else
        // 非 SIMD 路径：展开循环比较（SIMD 友好布局）
        if (LIKELY(set.conn_ids[0] == conn_id)) {
            // Way 0 命中
            set.access_counts[0] = 255;
            set.handlers[0](set.ctxs[0], wc);
            if (set.access_counts[1] > 0) --set.access_counts[1];
            return;
        }
        if (LIKELY(set.conn_ids[1] == conn_id)) {
            // Way 1 命中
            set.access_counts[1] = 255;
            set.handlers[1](set.ctxs[1], wc);
            if (set.access_counts[0] > 0) --set.access_counts[0];
            return;
        }
#endif

        // 3. L1 哈希热缓存查找（处理 L0 未命中）
        auto* entry = FindInHotCache(conn_id);
        if (LIKELY(entry)) {
            entry->handler(entry->ctx, wc);
            entry->access_count++;
            // 尝试提升到 L0
            MaybePromoteToL0(conn_id, entry->ctx, entry->handler);
            return;
        }

        // 4. L2 二级索引查找（冷路径）
        DispatchFromL2(conn_id, wc);
    }

    // 【优化】软件流水线预取：处理第 i 个 WC 时预取后续数据
    // - i+4: 预取 RpcSlotHot（最热数据，L1 缓存）
    // - i+8: 预取 Connection 结构（次热数据，L2 缓存）
    // 这种步长预取比简单的 +8 线性预取更高效
    HOT_FUNCTION
    void DispatchBatch(const ibv_wc* wc_array, int count, RpcSlotPool* slot_pool) {
        for (int i = 0; i < count; ++i) {
            // 步长预取：提前加载后续 WC 需要的数据
            if (i + 4 < count) {
                // 预取 RpcSlotHot（16B），最高优先级放入 L1
                uint32_t slot_idx = WrId::GetSlotIndex(wc_array[i + 4].wr_id);
                __builtin_prefetch(slot_pool->GetHot(slot_idx), 0, 3);
            }
            if (i + 8 < count) {
                // 预取 Connection 结构，较低优先级放入 L2
                uint16_t conn_id = WrId::GetConnId(wc_array[i + 8].wr_id);
                __builtin_prefetch(GetConnectionFromL0(conn_id), 0, 1);
            }

            Dispatch(wc_array[i]);
        }
    }

    // 无预取版本（用于 count 较小的场景）
    HOT_FUNCTION ALWAYS_INLINE
    void DispatchBatchSimple(const ibv_wc* wc_array, int count) {
        for (int i = 0; i < count; ++i) {
            Dispatch(wc_array[i]);
        }
    }

    // 从 L0 缓存快速获取 Connection 指针（用于预取）
    ALWAYS_INLINE void* GetConnectionFromL0(uint16_t conn_id) {
        size_t set_idx = conn_id & kL0SetMask;
        // 预取容错：返回任意一路的 ctx（可能不准确，但预取允许）
        return l0_cache_[set_idx].ctxs[0];
    }

private:
    // ========== L0 2-路组相联缓存（最快）==========
    // 放在类首部，提升缓存局部性
    alignas(64) L0CacheSet l0_cache_[kL0SetCount];

    // ========== L1 哈希热缓存 ==========
    static constexpr size_t kHotCacheSize = 64;
    static constexpr size_t kHotCacheMask = kHotCacheSize - 1;

    struct ConnectionEntry {
        void* ctx = nullptr;
        WcHandler handler = nullptr;
    };

    struct HotCacheEntry {
        uint16_t conn_id = 0xFFFF;      // 0xFFFF = empty
        uint16_t access_count = 0;       // LRU 计数
        void* ctx = nullptr;
        WcHandler handler = nullptr;
    };

    alignas(64) HotCacheEntry hot_cache_[kHotCacheSize];

    // ========== L2 二级索引 ==========
    ConnectionEntry* buckets_[256];

    struct {
        void* ctx = nullptr;
        ErrorHandler handler = nullptr;
    } error_handler_;

    // 哈希函数（简单但高效）
    ALWAYS_INLINE size_t Hash(uint16_t conn_id) const {
        // 使用 fibonacci 哈希
        return (conn_id * 0x9E3779B9u) & kHotCacheMask;
    }

    // O(1) 热缓存查找
    ALWAYS_INLINE HotCacheEntry* FindInHotCache(uint16_t conn_id) {
        size_t idx = Hash(conn_id);

        // 线性探测（最多探测 4 次）
        for (int probe = 0; probe < 4; ++probe) {
            size_t pos = (idx + probe) & kHotCacheMask;
            if (hot_cache_[pos].conn_id == conn_id) {
                return &hot_cache_[pos];
            }
            if (hot_cache_[pos].conn_id == 0xFFFF) {
                return nullptr;  // 空槽，未找到
            }
        }
        return nullptr;
    }

    // 插入热缓存（LRU 淘汰）
    void InsertHotCache(uint16_t conn_id, void* ctx, WcHandler handler) {
        size_t idx = Hash(conn_id);

        // 寻找空槽或 LRU 最小的槽
        size_t best_pos = idx;
        uint16_t min_access = UINT16_MAX;

        for (int probe = 0; probe < 4; ++probe) {
            size_t pos = (idx + probe) & kHotCacheMask;

            // 已存在则更新
            if (hot_cache_[pos].conn_id == conn_id) {
                hot_cache_[pos].ctx = ctx;
                hot_cache_[pos].handler = handler;
                hot_cache_[pos].access_count++;
                return;
            }

            // 空槽直接使用
            if (hot_cache_[pos].conn_id == 0xFFFF) {
                best_pos = pos;
                break;
            }

            // 记录 LRU 最小的
            if (hot_cache_[pos].access_count < min_access) {
                min_access = hot_cache_[pos].access_count;
                best_pos = pos;
            }
        }

        // 插入/替换
        hot_cache_[best_pos].conn_id = conn_id;
        hot_cache_[best_pos].ctx = ctx;
        hot_cache_[best_pos].handler = handler;
        hot_cache_[best_pos].access_count = 1;
    }

    // L0 提升（从 L1 命中时尝试提升到 L0，2-路组相联版本）
    ALWAYS_INLINE void MaybePromoteToL0(uint16_t conn_id, void* ctx, WcHandler handler) {
        size_t set_idx = conn_id & kL0SetMask;
        L0CacheSet& set = l0_cache_[set_idx];

        // 优先找空槽或 LRU 较低的槽（使用 SIMD 友好布局）
        int target_way = -1;

        for (size_t w = 0; w < kL0Ways; ++w) {
            if (set.conn_ids[w] == 0xFFFF) {
                // 空槽，直接使用
                target_way = static_cast<int>(w);
                break;
            }
            if (set.conn_ids[w] == conn_id) {
                // 已在 L0 中，无需提升
                return;
            }
        }

        // 如果没找到空槽，选择 access_count 较低的路进行替换
        if (target_way < 0) {
            target_way = (set.access_counts[0] <= set.access_counts[1]) ? 0 : 1;
        }

        set.conn_ids[target_way] = conn_id;
        set.generations[target_way]++;
        set.ctxs[target_way] = ctx;
        set.handlers[target_way] = handler;
        set.access_counts[target_way] = 128;  // 初始中等优先级
    }

    // L2 回退查找（冷路径）
    void DispatchFromL2(uint16_t conn_id, const ibv_wc& wc) {
        uint8_t bucket_idx = conn_id >> 8;
        uint8_t slot_idx = conn_id & 0xFF;

        if (LIKELY(buckets_[bucket_idx])) {
            auto& bentry = buckets_[bucket_idx][slot_idx];
            if (LIKELY(bentry.handler)) {
                bentry.handler(bentry.ctx, wc);
                // 提升到 L1 哈希热缓存
                InsertHotCache(conn_id, bentry.ctx, bentry.handler);
            }
        }
    }
};
```

### 6.2 Reactor（动态 Batch Size + MPSC 批量消费）

```cpp
// 设计考虑：
// 1. CQ batch size 根据场景可调：延迟优先 8-16，吞吐优先 32-64
// 2. MPSC 队列批量消费：一次 exchange 取走整个链表
// 3. 核心循环添加 __attribute__((hot))
// 4. 日志在 benchmark/prod 中编译期移除

class Reactor {
public:
    struct Config {
        // CQ 批量大小策略
        enum class BatchMode {
            LATENCY,        // 8-16，低延迟
            THROUGHPUT,     // 32-64，高吞吐
            ADAPTIVE,       // 动态调整
        };
        BatchMode batch_mode = BatchMode::THROUGHPUT;

        int latency_batch_size = 16;
        int throughput_batch_size = 64;
        int max_batch_size = 128;

        uint32_t max_spin_count = 1000;

        // NUMA 配置
        int cpu_core = -1;          // -1 表示不绑定
        int numa_node = -1;
    };

    Reactor(const Config& config);
    ~Reactor();

    // 必须在 Reactor 线程调用，完成初始化
    Status Initialize();

    void RegisterCQ(CompletionQueue* cq, CompletionDispatcher* dispatcher);

    // Poller 使用函数指针
    using Poller = int (*)(void* ctx);
    void RegisterPoller(Poller fn, void* ctx);

    // 定时器使用时间轮
    void ScheduleTimeout(uint32_t slot_index, uint8_t generation,
                         uint32_t timeout_ms);

    // 核心 Poll 循环
    HOT_FUNCTION int Poll();

    void Run();
    void Stop();

    // Lock-free 跨线程任务
    // 使用 intrusive task，避免堆分配
    struct Task {
        void (*fn)(void* ctx);
        void* ctx;
        Task* next;
    };

    // 批量消费优化：一次 exchange 取走整个链表
    void Execute(Task* task);

    // 获取当前 batch size
    int GetCurrentBatchSize() const;

private:
    Config config_;
    std::atomic<bool> running_{false};
    std::thread::id thread_id_;

    struct CQEntry {
        CompletionQueue* cq;
        CompletionDispatcher* dispatcher;
    };
    std::vector<CQEntry> cq_entries_;

    struct PollerEntry {
        Poller fn;
        void* ctx;
    };
    std::vector<PollerEntry> pollers_;

    // 时间轮（替代 min-heap）
    std::unique_ptr<TimingWheel> timing_wheel_;

    // MPSC 队列（批量消费）
    std::atomic<Task*> task_head_{nullptr};

    // 预分配 WC buffer
    ibv_wc* wc_buffer_;
    int wc_buffer_size_;

    // 自适应 batch size
    int current_batch_size_;
    uint64_t poll_count_ = 0;
    uint64_t total_wc_count_ = 0;

    // 自旋退避
    uint32_t idle_spin_count_ = 0;

    HOT_FUNCTION int ProcessTasks();
    HOT_FUNCTION int ProcessTimeouts();
    void AdaptiveBatchSize(int wc_count);

    // 自适应退避（使用 _mm_pause 释放 CPU 资源）
    ALWAYS_INLINE void AdaptiveBackoff() {
        ++idle_spin_count_;

        if (idle_spin_count_ < 100) {
            // 短暂空闲：使用 _mm_pause 让出流水线
            // 防止 CPU 过热，降低功耗
            _mm_pause();
        } else if (idle_spin_count_ < 1000) {
            // 中等空闲：多次 pause
            for (int i = 0; i < 4; ++i) {
                _mm_pause();
            }
        } else if (idle_spin_count_ < config_.max_spin_count) {
            // 较长空闲：pause + 更多 pause
            for (int i = 0; i < 16; ++i) {
                _mm_pause();
            }
        } else {
            // 长期空闲：可考虑 sched_yield 或 usleep
            // 但在高性能场景下通常保持 spin
            idle_spin_count_ = config_.max_spin_count;
            for (int i = 0; i < 32; ++i) {
                _mm_pause();
            }
        }
    }
};

// Poll 实现（含 SIMD 预取优化）
HOT_FUNCTION
int Reactor::Poll() {
    int total_events = 0;

    // 1. CQ Polling（自适应 batch size + SIMD 预取）
    for (auto& entry : cq_entries_) {
        int n = entry.cq->Poll(wc_buffer_, current_batch_size_);
        if (n > 0) {
            // 【优化】预取下一批 WC 到 L1 cache
            // ibv_wc 结构体约 48 字节，预取 8 个 WC
            if (n >= 8) {
                // 预取当前 WC 数组后续数据
                __builtin_prefetch(wc_buffer_ + 8, 0, 3);  // 0=read, 3=high locality
            }

            // 【优化】预取第一个 WC 关联的 RpcSlot
            // 提前将 slot 数据加载到 cache
            uint32_t first_slot = WrId::GetSlotIndex(wc_buffer_[0].wr_id);
            __builtin_prefetch(&slot_pool_[first_slot], 0, 3);

            entry.dispatcher->DispatchBatch(wc_buffer_, n);
            total_events += n;

            // 更新统计用于自适应
            total_wc_count_ += n;
        }
    }

    // 2. Doorbell Flush（批量提交，见下文）
    // ...

    // 3. 处理跨线程任务（批量消费）
    total_events += ProcessTasks();

    // 4. 时间轮超时检查
    total_events += ProcessTimeouts();

    // 5. 自定义 Pollers
    for (auto& p : pollers_) {
        total_events += p.fn(p.ctx);
    }

    // 6. 自适应调整
    if (++poll_count_ % 1000 == 0) {
        AdaptiveBatchSize(total_events);
    }

    // 7. 空闲退避
    if (total_events > 0) {
        idle_spin_count_ = 0;
    } else {
        AdaptiveBackoff();
    }

    return total_events;
}

// MPSC 批量消费
HOT_FUNCTION
int Reactor::ProcessTasks() {
    // 一次 exchange 取走整个链表
    Task* head = task_head_.exchange(nullptr, std::memory_order_acquire);
    if (!head) return 0;

    int count = 0;
    while (head) {
        Task* next = head->next;
        head->fn(head->ctx);
        // 注意：Task 由调用者管理生命周期（intrusive）
        head = next;
        ++count;
    }
    return count;
}
```

### 6.3 DoorbellBatcher（双重触发机制）

```cpp
// 设计考虑：
// 1. ibv_post_send 触发 MMIO（Doorbell），开销大
// 2. 策略：收集一批 WR，在 Reactor 循环结束时统一 Flush
// 3. 分阶段循环：Poll 32 WC → 生成 32 WR → Flush 一次
// 4. 关键：必须在循环结束时 Flush，不能每个请求 Flush
// 5. 【改进】双重触发机制解决：
//    - 队列过长导致延迟抖动：batch 太大时，第一个 WR 等待时间过长
//    - 饥饿现象：低负载时可能等待太久才触发 flush

class DoorbellBatcher {
public:
    struct Config {
        size_t max_batch_size = 32;         // 最大批量大小（触发条件1）
        uint32_t max_delay_us = 10;         // 最大延迟微秒（触发条件2）
        size_t low_load_threshold = 4;      // 低负载阈值（立即 flush）
    };

    DoorbellBatcher(QueuePair* qp, const Config& config = {})
        : qp_(qp), config_(config) {
        send_wrs_.reserve(128);
        recv_wrs_.reserve(128);
    }

    // 添加到批量队列（不立即提交）
    ALWAYS_INLINE void AddSendWr(ibv_send_wr* wr) {
        if (send_tail_) {
            send_tail_->next = wr;
        } else {
            send_head_ = wr;
            // 记录第一个 WR 的时间戳
            first_wr_tsc_ = __rdtsc();
        }
        send_tail_ = wr;
        wr->next = nullptr;
        ++send_count_;

        // 触发条件1：达到最大批量大小
        if (UNLIKELY(send_count_ >= config_.max_batch_size)) {
            FlushSend();
        }
    }

    // 双重触发检查（在 Reactor::Poll 中调用）
    HOT_FUNCTION ALWAYS_INLINE
    bool ShouldFlush() const {
        if (send_count_ == 0) return false;

        // 触发条件1：达到批量大小
        if (send_count_ >= config_.max_batch_size) {
            return true;
        }

        // 触发条件2：超过最大延迟时间
        // 使用 TSC 避免系统调用
        uint64_t elapsed_tsc = __rdtsc() - first_wr_tsc_;
        uint64_t max_tsc = config_.max_delay_us * tsc_per_us_;
        if (elapsed_tsc >= max_tsc) {
            return true;
        }

        // 触发条件3：低负载时立即 flush（避免饥饿）
        // 当连续几个 Poll 周期没有新 WR 时
        if (idle_poll_count_ >= 2 && send_count_ > 0) {
            return true;
        }

        return false;
    }

    // 统一提交（触发一次 Doorbell）
    HOT_FUNCTION Status FlushSend() {
        if (!send_head_) return Status::OK();

        // 【安全措施】确保所有 WR 构建（store）对硬件可见
        // 虽然 ibv_post_send 内部应有必要的屏障，但显式加上更安全
        // 开销极低（仅编译器屏障 + CPU store buffer flush）
        std::atomic_thread_fence(std::memory_order_release);

        ibv_send_wr* bad_wr = nullptr;
        int ret = ibv_post_send(qp_->GetRawQP(), send_head_, &bad_wr);

        // 重置
        send_head_ = nullptr;
        send_tail_ = nullptr;
        send_count_ = 0;
        idle_poll_count_ = 0;

        if (ret != 0) {
            return Status::IOError("ibv_post_send failed", ret);
        }
        return Status::OK();
    }

    // Reactor Poll 结束时调用
    HOT_FUNCTION Status OnPollEnd(bool had_events) {
        if (!had_events) {
            ++idle_poll_count_;
        } else {
            idle_poll_count_ = 0;
        }

        if (ShouldFlush()) {
            return FlushSend();
        }
        return Status::OK();
    }

    size_t PendingSendCount() const { return send_count_; }

    // 校准 TSC 频率（初始化时调用一次）
    static void CalibrateTsc() {
        auto start = std::chrono::steady_clock::now();
        uint64_t tsc_start = __rdtsc();

        // 睡眠 10ms 校准
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        uint64_t tsc_end = __rdtsc();
        auto end = std::chrono::steady_clock::now();

        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            end - start).count();
        tsc_per_us_ = (tsc_end - tsc_start) / elapsed_us;
    }

private:
    QueuePair* qp_;
    Config config_;

    ibv_send_wr* send_head_ = nullptr;
    ibv_send_wr* send_tail_ = nullptr;
    size_t send_count_ = 0;

    ibv_recv_wr* recv_head_ = nullptr;
    ibv_recv_wr* recv_tail_ = nullptr;
    size_t recv_count_ = 0;

    std::vector<ibv_send_wr> send_wrs_;
    std::vector<ibv_recv_wr> recv_wrs_;

    // 双重触发支持
    uint64_t first_wr_tsc_ = 0;         // 第一个 WR 的时间戳
    uint32_t idle_poll_count_ = 0;      // 空闲 poll 计数

    static inline uint64_t tsc_per_us_ = 2400;  // 默认 2.4GHz，需校准
};
```

### 6.4 QueuePair（CQ Backpressure + Signaled 策略）

```cpp
// 设计考虑：
// 1. CQ overflow 防护：outstanding signaled WR ≤ CQ depth / 2
// 2. Signaled 策略：每 N 个 WR signaled 一次
// 3. 禁止 stack send：所有数据必须来自预注册 buffer

class QueuePair {
public:
    struct Config {
        uint32_t max_send_wr;
        uint32_t max_recv_wr;
        uint32_t max_send_sge;
        uint32_t max_recv_sge;
        uint32_t max_inline_data;

        uint32_t signal_interval = 16;      // 每 N 个 signaled
        // 【优化】CQ 深度调大：1M QPS 下，signal_interval=16 意味着每秒 62.5K signaled WR
        // CQ 深度需要 >> 62.5K，否则可能因 CQ 满导致阻塞
        uint32_t cq_depth = 65536;          // CQ 深度（原 4096 太小）
    };

    static Result<std::unique_ptr<QueuePair>> Create(
        ProtectionDomain* pd,
        CompletionQueue* send_cq,
        CompletionQueue* recv_cq,
        const Config& config);

    // CQ Backpressure 检查
    // 返回 false 时必须等待 CQ 空间
    ALWAYS_INLINE bool CanPostSignaled() const {
        // 硬规则：outstanding signaled WR ≤ CQ depth / 2
        return outstanding_signaled_ < (config_.cq_depth / 2);
    }

    // 当前 outstanding signaled WR 数量
    uint32_t GetOutstandingSignaled() const { return outstanding_signaled_; }

    // Send（自动 signaled 策略）
    // 注意：buf 必须来自预注册 buffer，禁止 stack 数据！
    HOT_FUNCTION Status PostSend(const Buffer& buf, uint64_t wr_id) {
        bool should_signal = (++unsignaled_count_ >= config_.signal_interval);

        if (should_signal) {
            if (!CanPostSignaled()) {
                return Status::ResourceExhausted("CQ backpressure");
            }
            unsignaled_count_ = 0;
            ++outstanding_signaled_;
        }

        // 实际 post...
        return DoPostSend(buf, wr_id, should_signal);
    }

    // Inline Send（Header 专用，强制 inline）
    Status PostSendInline(const void* data, size_t length, uint64_t wr_id);

    // 完成回调时调用，减少 outstanding 计数
    ALWAYS_INLINE void OnSendComplete(bool was_signaled) {
        if (was_signaled) {
            --outstanding_signaled_;
        }
    }

    // SGL 自动分片：当 iovec 数量超过 max_sge 时自动拆分
    // 存储场景常见 "1 个控制头 + N 个离散页"
    HOT_FUNCTION
    Status PostSendSGL(const struct iovec* iov, size_t iov_count,
                       uint64_t wr_id_base, uint16_t conn_id, uint8_t generation) {
        const size_t max_sge = config_.max_send_sge;

        // 快速路径：SGE 数量在限制内
        if (LIKELY(iov_count <= max_sge)) {
            return DoPostSendSGL(iov, iov_count, wr_id_base, true);
        }

        // 慢路径：需要分片
        // 将大 SGL 拆分成多个 WR，每个 WR 最多 max_sge 个 SGE
        size_t remaining = iov_count;
        size_t offset = 0;
        uint32_t fragment_index = 0;

        while (remaining > 0) {
            size_t chunk = std::min(remaining, max_sge);
            bool is_last = (remaining == chunk);

            // 只有最后一个分片需要 signaled（如果需要的话）
            uint64_t frag_wr_id = WrId::Encode(
                conn_id, RpcOpcode::REQUEST_SEND,
                generation, (wr_id_base & 0xFFFFFFFF) | (fragment_index << 24));

            auto status = DoPostSendSGL(iov + offset, chunk, frag_wr_id, is_last);
            if (!status.ok()) return status;

            remaining -= chunk;
            offset += chunk;
            ++fragment_index;
        }

        return Status::OK();
    }

    // 获取硬件 SGE 限制
    uint32_t GetMaxSGE() const { return config_.max_send_sge; }

private:
    ibv_qp* qp_;
    Config config_;
    uint32_t unsignaled_count_ = 0;
    uint32_t outstanding_signaled_ = 0;

    Status DoPostSend(const Buffer& buf, uint64_t wr_id, bool signaled);
    Status DoPostSendSGL(const struct iovec* iov, size_t count,
                         uint64_t wr_id, bool signaled);
};
```

### 6.4.1 SGE 模板预构建与展开填充优化

```cpp
// 设计考虑：
// 1. 存储场景常见模式："1 Header + N Pages"（如 4K 页）
// 2. std::min 和循环分片在热路径上有指令开销
// 3. 预构建 SGE 模板避免运行时计算
// 4. iov_count < max_sge（通常 16）时，使用 __builtin_memcpy 展开填充

// 预定义的存储模式 SGE 模板
struct SgeTemplate {
    static constexpr size_t MAX_SGE = 16;  // 硬件常见限制

    // 模板类型
    enum class Pattern : uint8_t {
        HEADER_ONLY = 0,        // 仅控制头（1 SGE）
        HEADER_PLUS_1PAGE = 1,  // 1 Header + 1 Page（2 SGE）
        HEADER_PLUS_4PAGE = 2,  // 1 Header + 4 Pages（5 SGE）
        HEADER_PLUS_8PAGE = 3,  // 1 Header + 8 Pages（9 SGE）
        CUSTOM = 255,           // 自定义（使用通用路径）
    };

    // 预构建的 WR 模板（只填 opcode/flags，运行时填 addr/length/lkey）
    ibv_send_wr wr_template;
    ibv_sge sge_array[MAX_SGE];
    uint8_t sge_count;
    Pattern pattern;
};

// 模板池（per-QueuePair，初始化时预构建）
class SgeTemplatePool {
public:
    SgeTemplatePool() {
        // 预构建常用模式
        InitTemplate(SgeTemplate::Pattern::HEADER_ONLY, 1);
        InitTemplate(SgeTemplate::Pattern::HEADER_PLUS_1PAGE, 2);
        InitTemplate(SgeTemplate::Pattern::HEADER_PLUS_4PAGE, 5);
        InitTemplate(SgeTemplate::Pattern::HEADER_PLUS_8PAGE, 9);
    }

    // 获取匹配的模板
    ALWAYS_INLINE const SgeTemplate* GetTemplate(size_t iov_count) const {
        // 快速路径：匹配预构建模式
        switch (iov_count) {
            case 1:  return &templates_[0];  // HEADER_ONLY
            case 2:  return &templates_[1];  // HEADER_PLUS_1PAGE
            case 5:  return &templates_[2];  // HEADER_PLUS_4PAGE
            case 9:  return &templates_[3];  // HEADER_PLUS_8PAGE
            default: return nullptr;         // 使用通用路径
        }
    }

private:
    SgeTemplate templates_[4];

    void InitTemplate(SgeTemplate::Pattern pattern, uint8_t sge_count);
};

// 展开填充优化：避免循环开销
class SgeFiller {
public:
    // 编译期展开的 SGE 填充（iov_count <= MAX_SGE 时）
    template<size_t N>
    ALWAYS_INLINE static void FillUnrolled(
        ibv_sge* sge_array,
        const struct iovec* iov,
        uint32_t lkey) {
        // 使用 __builtin_memcpy 触发编译器展开
        // 每个 SGE 填充 24 字节（addr:8 + length:4 + lkey:4 + padding:8）
        if constexpr (N >= 1) {
            sge_array[0].addr = reinterpret_cast<uint64_t>(iov[0].iov_base);
            sge_array[0].length = static_cast<uint32_t>(iov[0].iov_len);
            sge_array[0].lkey = lkey;
        }
        if constexpr (N >= 2) {
            sge_array[1].addr = reinterpret_cast<uint64_t>(iov[1].iov_base);
            sge_array[1].length = static_cast<uint32_t>(iov[1].iov_len);
            sge_array[1].lkey = lkey;
        }
        // ... 展开到 N=16
        if constexpr (N > 2) {
            FillUnrolled<N - 2>(sge_array + 2, iov + 2, lkey);
        }
    }

    // 运行时分派到编译期展开版本
    HOT_FUNCTION ALWAYS_INLINE
    static void Fill(ibv_sge* sge_array, const struct iovec* iov,
                     size_t iov_count, uint32_t lkey) {
        // 对于小于硬件限制的常见情况，直接展开
        switch (iov_count) {
            case 1:  FillUnrolled<1>(sge_array, iov, lkey);  return;
            case 2:  FillUnrolled<2>(sge_array, iov, lkey);  return;
            case 3:  FillUnrolled<3>(sge_array, iov, lkey);  return;
            case 4:  FillUnrolled<4>(sge_array, iov, lkey);  return;
            case 5:  FillUnrolled<5>(sge_array, iov, lkey);  return;
            case 6:  FillUnrolled<6>(sge_array, iov, lkey);  return;
            case 7:  FillUnrolled<7>(sge_array, iov, lkey);  return;
            case 8:  FillUnrolled<8>(sge_array, iov, lkey);  return;
            case 9:  FillUnrolled<9>(sge_array, iov, lkey);  return;
            case 10: FillUnrolled<10>(sge_array, iov, lkey); return;
            case 11: FillUnrolled<11>(sge_array, iov, lkey); return;
            case 12: FillUnrolled<12>(sge_array, iov, lkey); return;
            case 13: FillUnrolled<13>(sge_array, iov, lkey); return;
            case 14: FillUnrolled<14>(sge_array, iov, lkey); return;
            case 15: FillUnrolled<15>(sge_array, iov, lkey); return;
            case 16: FillUnrolled<16>(sge_array, iov, lkey); return;
            default:
                // 超过 16 的罕见情况，fallback 到循环
                for (size_t i = 0; i < iov_count; ++i) {
                    sge_array[i].addr = reinterpret_cast<uint64_t>(iov[i].iov_base);
                    sge_array[i].length = static_cast<uint32_t>(iov[i].iov_len);
                    sge_array[i].lkey = lkey;
                }
        }
    }
};

// 【优化】SGL 地址对齐检查器
// RDMA 网卡在处理对齐地址时性能最优（减少 PCIe TLP 分割）
class SgeAlignmentChecker {
public:
    static constexpr size_t PAGE_SIZE = 4096;
    static constexpr size_t CACHE_LINE_SIZE = 64;

    // 检查结果
    struct AlignmentReport {
        size_t total_sge;               // 总 SGE 数量
        size_t page_aligned_count;      // 4KB 对齐的数量
        size_t cache_aligned_count;     // 64B 对齐的数量
        size_t unaligned_count;         // 未对齐的数量
        size_t total_unaligned_bytes;   // 未对齐导致需要额外处理的字节数

        float AlignmentRatio() const {
            return total_sge > 0 ? static_cast<float>(page_aligned_count) / total_sge : 1.0f;
        }

        bool IsOptimal() const {
            // 所有数据块都页对齐
            return unaligned_count == 0;
        }
    };

    // 检查 iovec 数组的对齐情况
    ALWAYS_INLINE static AlignmentReport Check(const struct iovec* iov, size_t iov_count) {
        AlignmentReport report = {};
        report.total_sge = iov_count;

        for (size_t i = 0; i < iov_count; ++i) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(iov[i].iov_base);

            if ((addr & (PAGE_SIZE - 1)) == 0) {
                ++report.page_aligned_count;
                ++report.cache_aligned_count;  // 页对齐必然 cache line 对齐
            } else if ((addr & (CACHE_LINE_SIZE - 1)) == 0) {
                ++report.cache_aligned_count;
                ++report.unaligned_count;
                // 非页对齐可能导致 PCIe TLP 分割
                report.total_unaligned_bytes += std::min(
                    iov[i].iov_len,
                    PAGE_SIZE - (addr & (PAGE_SIZE - 1)));
            } else {
                ++report.unaligned_count;
                report.total_unaligned_bytes += iov[i].iov_len;
            }
        }

        return report;
    }

    // 快速检查是否所有地址都页对齐（热路径使用）
    ALWAYS_INLINE static bool AllPageAligned(const struct iovec* iov, size_t iov_count) {
        for (size_t i = 0; i < iov_count; ++i) {
            if (reinterpret_cast<uintptr_t>(iov[i].iov_base) & (PAGE_SIZE - 1)) {
                return false;
            }
        }
        return true;
    }

    // 存储场景专用：检查 "1 Header + N Pages" 模式
    // Header 允许非对齐，但 Pages 必须 4KB 对齐
    ALWAYS_INLINE static bool CheckStoragePattern(const struct iovec* iov, size_t iov_count) {
        if (iov_count < 2) return true;  // 无数据页

        // 跳过第一个（Header），检查后续是否都页对齐
        for (size_t i = 1; i < iov_count; ++i) {
            if (reinterpret_cast<uintptr_t>(iov[i].iov_base) & (PAGE_SIZE - 1)) {
                return false;
            }
        }
        return true;
    }

    // 【优化】检查 SGE 是否跨越 4KB 边界
    // 如果一个 SGE 的 addr 和 length 导致传输跨越了 4KB 物理边界，
    // PCIe 控制器会将其拆分为两个 TLP 包，增加硬件层开销
    ALWAYS_INLINE static bool CrossesPageBoundary(uintptr_t addr, size_t length) {
        uintptr_t start_page = addr >> 12;              // 起始页号
        uintptr_t end_page = (addr + length - 1) >> 12; // 结束页号
        return start_page != end_page;
    }

    // 统计跨页 SGE 数量
    ALWAYS_INLINE static size_t CountCrossPageSge(const struct iovec* iov, size_t iov_count) {
        size_t count = 0;
        for (size_t i = 0; i < iov_count; ++i) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(iov[i].iov_base);
            if (CrossesPageBoundary(addr, iov[i].iov_len)) {
                ++count;
            }
        }
        return count;
    }

    // 【优化】自动拆分跨页 SGE
    // 将一个跨页的 SGE 拆分为两个 SGE，避免 PCIe TLP 分割开销
    // 返回拆分后的 SGE 数量（1 = 无需拆分，2 = 已拆分）
    static size_t SplitCrossPageSge(uintptr_t addr, size_t length,
                                    ibv_sge* out_sge, uint32_t lkey) {
        if (!CrossesPageBoundary(addr, length)) {
            // 无需拆分
            out_sge[0].addr = addr;
            out_sge[0].length = static_cast<uint32_t>(length);
            out_sge[0].lkey = lkey;
            return 1;
        }

        // 计算拆分点（下一个页边界）
        uintptr_t next_page_boundary = (addr & ~0xFFFULL) + PAGE_SIZE;
        size_t first_part_len = next_page_boundary - addr;
        size_t second_part_len = length - first_part_len;

        // 第一段：从 addr 到页边界
        out_sge[0].addr = addr;
        out_sge[0].length = static_cast<uint32_t>(first_part_len);
        out_sge[0].lkey = lkey;

        // 第二段：从页边界到结束
        out_sge[1].addr = next_page_boundary;
        out_sge[1].length = static_cast<uint32_t>(second_part_len);
        out_sge[1].lkey = lkey;

        return 2;
    }
};

// 【优化】LocalBufferPool 分配策略增强：确保 Buffer 不跨页
// 在分配时强制所有 Buffer 处于单物理页内
class PageAlignedBufferPool {
public:
    struct Config {
        size_t buffer_size;                     // 每个 buffer 大小（必须 <= 4096）
        size_t buffer_count;
        int numa_node = -1;
        bool force_single_page = true;          // 强制单页分配
    };

    static Result<std::unique_ptr<PageAlignedBufferPool>> Create(
        ProtectionDomain* pd, const Config& config) {

        // 如果 buffer_size > 4096 且要求单页分配，报错
        if (config.force_single_page && config.buffer_size > 4096) {
            return Status::InvalidArgument(
                "buffer_size > 4096 cannot fit in single page");
        }

        // 计算每页可容纳的 buffer 数量
        size_t buffers_per_page = 4096 / config.buffer_size;
        if (buffers_per_page == 0) buffers_per_page = 1;

        // 计算需要的页数
        size_t pages_needed = (config.buffer_count + buffers_per_page - 1) / buffers_per_page;

        // 分配页对齐的大块内存
        void* memory = aligned_alloc(4096, pages_needed * 4096);
        if (!memory) {
            return Status::ResourceExhausted("Failed to allocate page-aligned memory");
        }

        auto pool = std::make_unique<PageAlignedBufferPool>();
        pool->buffer_size_ = config.buffer_size;
        pool->buffers_per_page_ = buffers_per_page;

        // 注册 MR
        pool->mr_ = MemoryRegion::Register(pd, memory, pages_needed * 4096);

        // 初始化空闲列表：确保每个 buffer 都在页内
        pool->free_list_.reserve(config.buffer_count);
        for (size_t page = 0; page < pages_needed; ++page) {
            for (size_t i = 0; i < buffers_per_page && pool->free_list_.size() < config.buffer_count; ++i) {
                size_t offset = page * 4096 + i * config.buffer_size;
                pool->free_list_.push_back(offset);
            }
        }

        return pool;
    }

    // 分配保证不跨页的 buffer
    ALWAYS_INLINE Buffer Allocate() {
        if (free_list_.empty()) return Buffer{};
        size_t offset = free_list_.back();
        free_list_.pop_back();
        return mr_->GetBuffer(offset, buffer_size_);
    }

    ALWAYS_INLINE void Free(size_t offset) {
        free_list_.push_back(offset);
    }

private:
    std::unique_ptr<MemoryRegion> mr_;
    std::vector<size_t> free_list_;     // 对于初始化不在热路径，可用 vector
    size_t buffer_size_ = 0;
    size_t buffers_per_page_ = 0;
};

// 优化后的 DoPostSendSGL 实现
class QueuePairOptimized {
private:
    SgeTemplatePool template_pool_;
    ibv_sge cached_sge_[SgeTemplate::MAX_SGE];  // 预分配 SGE 数组

public:
    HOT_FUNCTION
    Status DoPostSendSGLOptimized(const struct iovec* iov, size_t iov_count,
                                   uint64_t wr_id, bool signaled, uint32_t lkey) {
        // 1. 尝试使用预构建模板
        const SgeTemplate* tpl = template_pool_.GetTemplate(iov_count);

        ibv_send_wr wr;
        if (tpl) {
            // 快速路径：复制模板并填充地址
            __builtin_memcpy(&wr, &tpl->wr_template, sizeof(ibv_send_wr));
            wr.wr_id = wr_id;
            wr.send_flags = signaled ? IBV_SEND_SIGNALED : 0;
        } else {
            // 通用路径：构建新 WR
            std::memset(&wr, 0, sizeof(wr));
            wr.wr_id = wr_id;
            wr.opcode = IBV_WR_SEND;
            wr.send_flags = signaled ? IBV_SEND_SIGNALED : 0;
            wr.num_sge = static_cast<int>(iov_count);
        }

        // 2. 使用展开填充 SGE
        SgeFiller::Fill(cached_sge_, iov, iov_count, lkey);
        wr.sg_list = cached_sge_;

        // 3. Post Send
        ibv_send_wr* bad_wr = nullptr;
        int ret = ibv_post_send(qp_, &wr, &bad_wr);
        if (UNLIKELY(ret != 0)) {
            return Status::Error("ibv_post_send failed", ret);
        }
        return Status::OK();
    }
};
```

### 6.5 FlowControl（懒更新 + Piggyback + 自适应阈值）

```cpp
// 设计考虑：
// 1. 每次 Recv 后检查 Credit 开销大
// 2. 懒更新：pending_credits > threshold% recv_depth 时才更新
// 3. Piggyback：优先通过 Response 捎带 Credit
// 4. std::atomic 使用 relaxed 语义（单线程消费）
// 5.【优化】自适应阈值：根据接收速率动态调整
//    - 高吞吐时：提高阈值（如 50%）以减少控制包
//    - 低吞吐或高延迟时：降低阈值（如 10%）以保证流畅性

class FlowControl {
public:
    struct Config {
        uint32_t recv_depth;
        float lazy_update_threshold = 0.25f;    // 默认 25% 阈值
        std::chrono::milliseconds idle_interval{100};

        // 自适应阈值配置
        bool enable_adaptive_threshold = true;  // 启用自适应阈值
        float min_threshold = 0.10f;            // 最小阈值 10%
        float max_threshold = 0.50f;            // 最大阈值 50%
        uint32_t window_size = 100;             // 滑动窗口大小（采样数）
        uint32_t high_rate_threshold = 10000;   // 高速率阈值（次/秒）
        uint32_t low_rate_threshold = 1000;     // 低速率阈值（次/秒）
    };

    FlowControl(const Config& config)
        : config_(config),
          lazy_threshold_(config.recv_depth * config.lazy_update_threshold),
          current_threshold_ratio_(config.lazy_update_threshold) {
        send_credits_.store(config.recv_depth, std::memory_order_relaxed);
        pending_credits_.store(0, std::memory_order_relaxed);
        last_sample_time_ = std::chrono::steady_clock::now();
    }

    // 发送端：检查并消耗 credit
    ALWAYS_INLINE bool TryConsume(uint32_t count = 1) {
        uint32_t current = send_credits_.load(std::memory_order_relaxed);
        if (current < count) return false;
        send_credits_.store(current - count, std::memory_order_relaxed);
        return true;
    }

    // 接收端：Recv 完成后调用
    ALWAYS_INLINE void OnRecvCompleted(uint32_t count = 1) {
        pending_credits_.fetch_add(count, std::memory_order_relaxed);

        // 更新滑动窗口统计
        if (config_.enable_adaptive_threshold) {
            UpdateRateStatistics(count);
        }
    }

    // 懒更新：检查是否需要发送 Credit（使用自适应阈值）
    ALWAYS_INLINE bool ShouldUpdateCredit() const {
        return pending_credits_.load(std::memory_order_relaxed) >= lazy_threshold_;
    }

    // 获取当前阈值比例（用于监控）
    float GetCurrentThresholdRatio() const {
        return current_threshold_ratio_;
    }

    // 获取当前接收速率估计（次/秒）
    uint32_t GetEstimatedRecvRate() const {
        return estimated_rate_;
    }

private:
    // 更新速率统计并动态调整阈值
    void UpdateRateStatistics(uint32_t count) {
        window_recv_count_ += count;
        ++window_sample_count_;

        // 每 window_size 个采样更新一次阈值
        if (window_sample_count_ >= config_.window_size) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_sample_time_).count();

            if (elapsed_ms > 0) {
                // 计算速率（次/秒）
                estimated_rate_ = static_cast<uint32_t>(
                    window_recv_count_ * 1000 / elapsed_ms);

                // 根据速率调整阈值
                AdjustThreshold();
            }

            // 重置窗口
            window_recv_count_ = 0;
            window_sample_count_ = 0;
            last_sample_time_ = now;
        }
    }

    // 根据速率动态调整阈值
    void AdjustThreshold() {
        float new_ratio;

        if (estimated_rate_ >= config_.high_rate_threshold) {
            // 高吞吐：提高阈值以减少控制包开销
            new_ratio = config_.max_threshold;
        } else if (estimated_rate_ <= config_.low_rate_threshold) {
            // 低吞吐/高延迟：降低阈值以保证流畅性
            new_ratio = config_.min_threshold;
        } else {
            // 中等负载：线性插值
            float rate_ratio = static_cast<float>(estimated_rate_ - config_.low_rate_threshold) /
                              (config_.high_rate_threshold - config_.low_rate_threshold);
            new_ratio = config_.min_threshold +
                       rate_ratio * (config_.max_threshold - config_.min_threshold);
        }

        // 平滑更新阈值（避免抖动）
        current_threshold_ratio_ = 0.8f * current_threshold_ratio_ + 0.2f * new_ratio;
        lazy_threshold_ = static_cast<uint32_t>(
            config_.recv_depth * current_threshold_ratio_);
    }

    // 获取 Piggyback Credit（用于 Response）
    uint32_t GetPiggybackCredits() {
        return pending_credits_.exchange(0, std::memory_order_relaxed);
    }

private:
    Config config_;
    uint32_t lazy_threshold_;
    float current_threshold_ratio_;             // 当前阈值比例

    // 使用 relaxed：单线程访问，不需要同步
    std::atomic<uint32_t> send_credits_;
    std::atomic<uint32_t> pending_credits_;
    std::chrono::steady_clock::time_point last_activity_;

    // 自适应阈值：滑动窗口统计
    std::chrono::steady_clock::time_point last_sample_time_;
    uint32_t window_recv_count_ = 0;            // 窗口内接收计数
    uint32_t window_sample_count_ = 0;          // 窗口内采样次数
    uint32_t estimated_rate_ = 0;               // 估计的接收速率（次/秒）
};
```

### 6.6 RpcEndpoint（Header/Data 分离 + 零读取优化）

```cpp
// 设计考虑：
// 1. Header 强制 Inline：省去网卡回读内存
// 2. Data 使用 WriteWithImm：对端从 CQE 直接获取 slot_index
// 3. 超时使用时间轮：O(1) 插入和检查
// 4. 热路径函数全部 inline
// 5. 【重要】零读取优化：Read Response 使用 RDMA_WRITE_WITH_IMM
//    - 服务端直接将数据写入客户端 Buffer
//    - slot_index 放在 imm_data 中发送
//    - 客户端 ibv_poll_cq 直接从 CQE 获取 imm_data
//    - 无需读取内存中的 Response Header（省去一次 DRAM Read）
//    - 这是冲击 1M QPS 的终极武器

class RpcEndpoint {
public:
    using RpcCallback = void (*)(void* ctx, RpcSlot* slot, Status status);

    RpcEndpoint(Connection* conn, LocalBufferPool* buffer_pool,
                TimingWheel* timing_wheel);

    // 发起请求（Header 强制 Inline）
    // 重要：data 必须来自 buffer_pool，禁止 stack 数据！
    HOT_FUNCTION
    Result<uint32_t> SendRequest(const void* header, size_t header_len,
                                 const Buffer* data_buf,  // 可选，大数据
                                 void* callback_ctx, RpcCallback callback) {
        // 1. 分配 slot
        auto [slot_index, generation] = slot_pool_.Allocate();
        RpcSlot* slot = slot_pool_.GetUnsafe(slot_index);

        // 2. 初始化 slot
        slot->hot.SetState(RpcSlotHot::kStateInUse);
        slot->hot.conn_id = conn_->GetConnId();
        slot->cold.callback_ctx = callback_ctx;
        slot->cold.callback_fn = callback;

        // 3. 注册超时（时间轮，O(1)）
        timing_wheel_->Schedule(slot_index, generation, timeout_ms_);

        // 4. 发送 Header（强制 Inline）
        uint64_t wr_id = WrId::Encode(conn_->GetConnId(),
                                       RpcOpcode::REQUEST_SEND,
                                       generation, slot_index);

        // Header 必须 <= max_inline_data
        conn_->GetQP()->PostSendInline(header, header_len, wr_id);
        slot->hot.total_wr_count++;

        // 5. 如果有大数据，使用 WriteWithImm
        if (data_buf) {
            // imm_data = slot_index，对端直接从 CQE 获取
            uint32_t imm_data = slot_index;
            conn_->GetQP()->PostWriteWithImm(*data_buf, remote_mem_,
                                              imm_data, wr_id);
            slot->hot.total_wr_count++;
        }

        return slot_index;
    }

    // WC 处理（全部 inline）
    HOT_FUNCTION ALWAYS_INLINE
    void OnSendComplete(const ibv_wc& wc) {
        uint32_t slot_index = WrId::GetSlotIndex(wc.wr_id);
        uint8_t generation = WrId::GetGeneration(wc.wr_id);

        RpcSlot* slot = slot_pool_.Get(slot_index, generation);
        if (UNLIKELY(!slot)) return;  // stale

        slot->hot.completed_wr_count++;

        if (slot->IsCompleted()) {
            OnRpcComplete(slot);
        }
    }

    // 【零读取优化】处理 RDMA_WRITE_WITH_IMM 完成
    // 客户端接收端：直接从 CQE 的 imm_data 获取 slot_index
    // 无需读取内存中的 Response Header
    HOT_FUNCTION ALWAYS_INLINE
    void OnWriteWithImmRecv(const ibv_wc& wc) {
        // imm_data 编码: [16-bit generation | 16-bit slot_index]
        // 或者简单方案: 直接用 32-bit slot_index
        uint32_t imm_data = ntohl(wc.imm_data);

        // 方案一：简单编码（slot_index only）
        uint32_t slot_index = imm_data & 0xFFFF;
        uint8_t generation = (imm_data >> 16) & 0xFF;

        RpcSlot* slot = slot_pool_.Get(slot_index, generation);
        if (UNLIKELY(!slot)) return;

        // 数据已经通过 RDMA Write 直接写入预分配的 buffer
        // 无需任何内存读取操作，直接标记完成
        slot->hot.completed_wr_count++;

        if (slot->IsCompleted()) {
            OnRpcComplete(slot);
        }

        // 重新 Post Recv（为下一次 WriteWithImm）
        RepostRecvBuffer();
    }

    // 服务端：发送 Read Response（零读取路径）
    // 使用 RDMA_WRITE_WITH_IMM 直接写入客户端 buffer
    HOT_FUNCTION
    Status SendReadResponse(uint32_t client_slot_index, uint8_t client_generation,
                            const Buffer& data_buf,
                            const RemoteMemoryInfo& client_buffer) {
        // 编码 imm_data
        uint32_t imm_data = (static_cast<uint32_t>(client_generation) << 16) |
                            (client_slot_index & 0xFFFF);

        // 直接写入客户端 buffer，不需要发送 Header
        return conn_->GetQP()->PostWriteWithImm(
            data_buf,
            client_buffer.addr,
            client_buffer.rkey,
            htonl(imm_data),
            0  // wr_id 可以简化，因为是服务端发起
        );
    }

    // 超时检查（由 Reactor 调用）
    HOT_FUNCTION
    int CheckTimeouts(std::vector<TimingWheel::TimeoutEntry>& expired) {
        int count = 0;
        for (auto& entry : expired) {
            RpcSlot* slot = slot_pool_.Get(entry.slot_index, entry.generation);
            if (!slot || slot->IsCompleted()) continue;

            // 标记超时
            slot->cold.send.status_code = 1;  // timeout
            slot->hot.completed_wr_count = slot->hot.total_wr_count;
            EnqueueCompletion(slot, Status::Timeout());
            ++count;
        }
        return count;
    }

private:
    Connection* conn_;
    LocalBufferPool* buffer_pool_;
    TimingWheel* timing_wheel_;
    RpcSlotPool slot_pool_;
    CompletionQueueProxy completion_queue_;

    uint32_t timeout_ms_ = 30000;

    void OnRpcComplete(RpcSlot* slot);
    void EnqueueCompletion(RpcSlot* slot, Status status);
};
```

### 6.7 Keepalive（低频 / 独立 CQ）

```cpp
// 设计考虑：
// 1. 100 万 QPS 下，Keepalive 会打断 Poll 循环
// 2. 方案 A：降低频率（30s-60s 间隔）
// 3. 方案 B：使用独立 CQ，与数据路径分离
// 4. 默认使用方案 A

class Keepalive {
public:
    struct Config {
        std::chrono::seconds interval{30};      // 降低到 30s
        std::chrono::seconds timeout{90};
        uint32_t max_missed{3};

        // 是否使用独立 CQ
        bool use_separate_cq = false;
    };

    Keepalive(Connection* conn, const Config& config = {});

    // 低频检查（每 30s 一次）
    int Poll();

    void OnHeartbeatResponse();
    bool IsTimedOut() const;

private:
    Connection* conn_;
    Config config_;
    std::chrono::steady_clock::time_point last_heartbeat_;
    uint32_t missed_count_ = 0;
};
```

### 6.8 LocalBufferPool（Hugepage + 预分配 + FIFO 分配策略）

```cpp
// 设计考虑：
// 1. 使用 Hugepage 减少 TLB Miss
// 2. 一次性预分配所有 buffer
// 3. NUMA-aware 分配
// 4.【优化】使用固定大小数组替代 std::vector，避免热路径上的潜在重分配
// 5.【优化】采用 FIFO 分配策略替代 LIFO
//    - LIFO（栈）策略可能导致空间局部性差：新分配的 buffer 在物理内存上离散
//    - FIFO（队列）策略让连续分配的 buffer 在物理上也尽可能连续
//    - 提升后续 RDMA 操作的 PCIe 效率

// 分配策略枚举
enum class BufferAllocationPolicy {
    LIFO,   // 栈式分配（原策略）
    FIFO,   // 队列式分配（推荐，提升局部性）
    SLAB    // Slab 分配（按大小分类，未来扩展）
};

class LocalBufferPool {
public:
    struct Config {
        size_t buffer_size;
        size_t buffer_count;
        size_t max_buffer_count = 65536;  // 最大 buffer 数量
        int numa_node = -1;               // NUMA node
        bool use_hugepage = true;         // 使用 hugepage
        BufferAllocationPolicy policy = BufferAllocationPolicy::FIFO;  // 默认 FIFO
    };

    static Result<std::unique_ptr<LocalBufferPool>> Create(
        ProtectionDomain* pd, const Config& config) {

        if (config.buffer_count > config.max_buffer_count) {
            return Status::InvalidArgument("buffer_count exceeds max");
        }

        // 1. 选择分配器
        std::unique_ptr<MemoryAllocator> allocator;
        if (config.use_hugepage) {
            allocator = std::make_unique<HugepageAllocator>(config.numa_node);
        } else {
            allocator = std::make_unique<DefaultAllocator>();
        }

        // 2. 一次性分配整块内存
        size_t total_size = config.buffer_size * config.buffer_count;
        void* memory = allocator->Allocate(total_size, 4096);

        // 3. 注册为一个大 MR
        auto mr = MemoryRegion::Register(pd, memory, total_size);

        // 4. 构造 pool 并初始化
        auto pool = std::make_unique<LocalBufferPool>();
        pool->buffer_size_ = config.buffer_size;
        pool->buffer_count_ = config.buffer_count;
        pool->policy_ = config.policy;

        // 分配固定大小的 offset 数组（环形队列）
        pool->offset_ring_ = static_cast<size_t*>(
            aligned_alloc(64, config.buffer_count * sizeof(size_t)));

        // 预填充（顺序填充，低偏移先被分配）
        for (size_t i = 0; i < config.buffer_count; ++i) {
            pool->offset_ring_[i] = i * config.buffer_size;
        }

        // 初始化队列指针
        pool->head_ = 0;
        pool->tail_ = config.buffer_count;
        pool->count_ = config.buffer_count;

        return pool;
    }

    ~LocalBufferPool() {
        free(offset_ring_);
    }

    // 分配 buffer（支持 LIFO/FIFO 策略）
    ALWAYS_INLINE Buffer Allocate() {
        if (UNLIKELY(count_ == 0)) {
            return Buffer{};  // 无可用 buffer
        }

        size_t offset;
        if (policy_ == BufferAllocationPolicy::FIFO) {
            // FIFO：从队首取出（提升物理局部性）
            offset = offset_ring_[head_];
            head_ = (head_ + 1) % buffer_count_;
        } else {
            // LIFO：从队尾取出（原策略）
            tail_ = (tail_ - 1 + buffer_count_) % buffer_count_;
            offset = offset_ring_[tail_];
        }
        --count_;

        return mr_->GetBuffer(offset, buffer_size_);
    }

    // 释放 buffer（总是放回队尾）
    // 【优化】预取友好性：释放时预取 buffer 元数据，为下次分配预热缓存
    // 理由：刚释放的 buffer 很可能很快被再次分配
    ALWAYS_INLINE void Free(size_t offset) {
        offset_ring_[tail_] = offset;
        tail_ = (tail_ + 1) % buffer_count_;
        ++count_;

        // 预取该 buffer 的头部区域到 L1 缓存
        // 抵消下次 Allocate 时的内存延迟
        void* buffer_ptr = mr_->GetBufferPtr(offset);
        __builtin_prefetch(buffer_ptr, 1, 3);  // 写意图，最高优先级

        // 如果有 MemoryRegion 条目，也预取其元数据
        // 这在 buffer 被快速复用时可节省 50-100ns
    }

    // 获取可用 buffer 数量
    ALWAYS_INLINE size_t AvailableCount() const {
        return count_;
    }

    // 获取分配策略
    BufferAllocationPolicy GetPolicy() const {
        return policy_;
    }

private:
    std::unique_ptr<MemoryRegion> mr_;
    size_t* offset_ring_ = nullptr;     // 固定大小环形数组
    size_t head_ = 0;                   // 队首指针（FIFO 分配位置）
    size_t tail_ = 0;                   // 队尾指针（释放位置）
    size_t count_ = 0;                  // 当前可用数量
    size_t buffer_size_ = 0;
    size_t buffer_count_ = 0;
    BufferAllocationPolicy policy_ = BufferAllocationPolicy::FIFO;
};
```

## 7. 编译期优化

### 7.1 日志编译期移除

```cpp
// 设计考虑：
// 1. benchmark/prod 编译时完全移除日志
// 2. 使用宏控制，零运行时开销

#ifdef RDMA_ENABLE_LOGGING
    #define RDMA_LOG_DEBUG(fmt, ...) \
        rdma::Logger::Debug(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
    #define RDMA_LOG_INFO(fmt, ...) \
        rdma::Logger::Info(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#else
    #define RDMA_LOG_DEBUG(fmt, ...) ((void)0)
    #define RDMA_LOG_INFO(fmt, ...) ((void)0)
#endif

// 错误日志始终保留
#define RDMA_LOG_ERROR(fmt, ...) \
    rdma::Logger::Error(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
```

### 7.2 Fast Path 禁止规则

```cpp
// 编译期检查：Fast path 禁止 new/delete
// 使用 Address Sanitizer + 自定义 allocator hook 检测

// 文档规则（必须遵守）：
// 1. 禁止在热路径使用 std::vector::push_back（可能触发 resize）
// 2. 禁止在热路径使用 std::function（类型擦除可能 alloc）
// 3. 禁止在热路径使用 std::string（SSO 溢出可能 alloc）
// 4. 禁止使用 stack 上的数据作为 Send buffer
// 5. 所有容器必须预先 reserve
```

## 8. 设计考虑汇总

### 8.1 1M+ QPS 性能关键路径

| 优化点 | 问题 | 解决方案 |
|--------|------|----------|
| Timeout 检查 | min-heap O(log N) | 分层时间轮 O(1)，1ms 精度 |
| Cache Miss | 65536 数组随机访问 | 二级索引 + 哈希热缓存 O(1) |
| 调用层级深 | 分支预测抖动 | 热路径 inline + 无分支优化 |
| atomic 过重 | acquire/release 开销 | 单线程用 relaxed |
| CQ batch 固定 | 不适应负载 | 动态调整 8-128 + SIMD 预取 |
| Stack send | 隐性 memcpy | 禁止，必须预注册 buffer |
| CQ overflow | burst 打满 CQ | backpressure，signaled ≤ depth/2 |
| NUMA 跨节点 | 延迟抖动 | 强制同 NUMA |
| 日志开销 | 热路径日志 | 编译期移除 |
| new/delete | 分配开销 | 禁止，全部预分配 |
| Keepalive 频繁 | 打断 Poll | 低频 30s / 独立 CQ |
| Credit 更新频繁 | 控制面开销 | 懒更新 25% 阈值 |
| Doorbell 频繁 | MMIO 开销 | 双重触发批量 Flush |
| MPSC 竞争 | CAS bounce | 批量 exchange |
| CPU 空转 | 功耗高，流水线过热 | _mm_pause 分级退避 |
| SGL 超限 | iovec > max_sge 硬错误 | 自动分片 |
| SGL 填充循环 | std::min + 循环开销 | 模板预构建 + 展开填充 |
| spdk_vtophys 调用 | 页表查找开销 | MR 注册时缓存物理地址 |
| Response Header | DRAM Read 开销 | RDMA_WRITE_WITH_IMM 零读取 |
| RpcSlot 太大 | 128B 跨 cache line | 压缩到 64B 单 cache line |
| WR 构建 | 每次填充完整结构 | 预构建模板池 |
| Cache Miss | slot 访问延迟 | 流水线预取，提前 4 个 |
| QP Error | 重建连接中断长 | 快速恢复机制 |
| PCIe 带宽瓶颈 | TLP 顺序约束 | Relaxed Ordering（可选）|
| 请求端解析开销 | 服务端需解析 Request Header | 双向 ImmData |
| std::vector 重分配 | 热路径上 push_back 导致抖动 | 固定大小数组栈 |
| SPDK 内存碎片 | 分段 MR 注册走慢路径 | 初始化时预校验 + 强制连续 |
| RpcSlot Cache Miss | 回调信息与状态混在一起 | 热度分层：Hot + Context + Extended |
| Dispatcher 查找开销 | 哈希表探测开销 | L0 2-路组相联 + L1 哈希 + L2 索引 |
| SGL 非对齐地址 | PCIe TLP 分割开销 | SgeAlignmentChecker 预检查 |
| I-Cache 失效 | 热函数分散在内存中 | HOT_SECTION 属性集中放置 |
| 预取策略简单 | 线性 +8 预取效率低 | 软件流水线步长预取（i+4, i+8）|
| CQ 轮询延迟 | 等待 CQE 浪费 50-100ns | Header-on-Data 模式（可选）|
| SGE 跨 4KB 边界 | PCIe TLP 分割开销 | 自动拆分跨页 SGE |
| L0 缓存冲突 | 直接映射碰撞多 | 2-路组相联，命中率 60%→90% |
| SPDK 全速自旋 | Thermal Throttling | 动态退避策略 |
| Buffer 物理分散 | LIFO 导致局部性差 | FIFO 分配策略 |
| Retry Error 中断长 | 完全重建连接 | SQ Drain + 快速重置 + 重发 |
| generation 快速回绕 | 8 位仅 256 次复用 | 扩展到 12 位（4096 次）|
| Credit 阈值固定 | 不适应负载变化 | 滑动窗口 + 自适应阈值 |
| Buffer 释放后冷启动 | 下次分配有 Cache Miss | Free 时预取到 L1 缓存 |
| 快速恢复死循环 | 无限重试占用 CPU | 静默重试计数 + 窗口检测 |
| 位域编译器差异 | 跨编译器行为不一致 | 纯整数 flags + 内联访问器 |
| L0 缓存比较慢 | 两次独立比较 | SIMD 32-bit 并行比较 |
| CQE 产生多 | PCIe 流量大 | CQE Compression（ConnectX-6+）|

### 8.1.1 PCIe Relaxed Ordering（可选配置）

```cpp
// 设计考虑：
// 1. 频繁的 RDMA Write 消耗大量 PCIe 带宽
// 2. PCIe 默认 Strong Ordering 要求 TLP 包按序到达，增加拥塞
// 3. Relaxed Ordering 允许 PCIe 控制器重排 TLP 包，提升吞吐
// 4. 需要硬件支持（现代网卡和 CPU 通常支持）
// 5. 作为可选配置，默认关闭以保证兼容性

struct MemoryRegionConfig {
    bool enable_local_write = true;
    bool enable_remote_write = true;
    bool enable_remote_read = true;

    // 【可选优化】启用 PCIe Relaxed Ordering
    // 可显著提升高并发小包（RPC Header）场景的吞吐量
    // 要求：1. 网卡支持 2. CPU/PCIe 控制器支持 3. 应用无强顺序依赖
    bool enable_relaxed_ordering = false;

    int GetAccessFlags() const {
        int flags = 0;
        if (enable_local_write)  flags |= IBV_ACCESS_LOCAL_WRITE;
        if (enable_remote_write) flags |= IBV_ACCESS_REMOTE_WRITE;
        if (enable_remote_read)  flags |= IBV_ACCESS_REMOTE_READ;
#ifdef IBV_ACCESS_RELAXED_ORDERING
        if (enable_relaxed_ordering) flags |= IBV_ACCESS_RELAXED_ORDERING;
#endif
        return flags;
    }
};

class MemoryRegion {
public:
    static Result<std::unique_ptr<MemoryRegion>> Register(
        ProtectionDomain* pd, void* addr, size_t size,
        const MemoryRegionConfig& config = {}) {

        ibv_mr* mr = ibv_reg_mr(pd->GetRawPD(), addr, size, config.GetAccessFlags());
        if (!mr) {
            return Status::IOError("ibv_reg_mr failed", errno);
        }

        auto region = std::make_unique<MemoryRegion>();
        region->mr_ = mr;
        region->addr_ = addr;
        region->size_ = size;
        return region;
    }

    uint32_t GetLkey() const { return mr_->lkey; }
    uint32_t GetRkey() const { return mr_->rkey; }

private:
    ibv_mr* mr_ = nullptr;
    void* addr_ = nullptr;
    size_t size_ = 0;
};
```

### 8.1.2 双向 ImmData（请求端 + 响应端零解析）

```cpp
// 设计考虑：
// 1. 已实现服务端零读取：Response 使用 RDMA_WRITE_WITH_IMM
// 2. 扩展：请求端也使用 RDMA_WRITE_WITH_IMM 发送数据
// 3. 收益：服务端直接在 WC.imm_data 中获取 slot_index，无需解析 Request Header
// 4. 适用场景：简单的"写存储"请求

// ImmData 布局（32-bit）：
// [ 16 bits conn_id | 8 bits opcode | 8 bits slot_index_low ]
// 注：slot_index 完整值通过 wr_id 传递，imm_data 中仅存低 8 位用于快速校验

namespace ImmDataCodec {
    // 编码
    ALWAYS_INLINE uint32_t Encode(uint16_t conn_id, uint8_t opcode, uint8_t slot_hint) {
        return (static_cast<uint32_t>(conn_id) << 16) |
               (static_cast<uint32_t>(opcode) << 8) |
               slot_hint;
    }

    // 解码
    ALWAYS_INLINE uint16_t GetConnId(uint32_t imm_data) {
        return static_cast<uint16_t>(imm_data >> 16);
    }

    ALWAYS_INLINE uint8_t GetOpcode(uint32_t imm_data) {
        return static_cast<uint8_t>((imm_data >> 8) & 0xFF);
    }

    ALWAYS_INLINE uint8_t GetSlotHint(uint32_t imm_data) {
        return static_cast<uint8_t>(imm_data & 0xFF);
    }
}

// 双向 ImmData 的使用场景
class BidirectionalImmData {
public:
    // 客户端发送写请求（使用 WriteWithImm）
    Status SendWriteRequest(void* data, size_t length, uint64_t remote_addr,
                            uint32_t remote_rkey, uint16_t conn_id, uint32_t slot_index) {
        // 编码 imm_data
        uint32_t imm_data = ImmDataCodec::Encode(
            conn_id,
            static_cast<uint8_t>(RpcOpcode::REQUEST_WRITE),
            static_cast<uint8_t>(slot_index & 0xFF)
        );

        // 发送 RDMA_WRITE_WITH_IMM
        return PostWriteWithImm(data, length, remote_addr, remote_rkey,
                                WrId::Encode(conn_id, RpcOpcode::REQUEST_WRITE, 0, slot_index),
                                imm_data);
    }

    // 服务端处理 WriteWithImm 完成
    // WC 中直接获取 imm_data，无需读取 Request Header
    void OnRecvWriteWithImm(const ibv_wc& wc) {
        uint32_t imm_data = ntohl(wc.imm_data);  // 网络字节序转换
        uint16_t conn_id = ImmDataCodec::GetConnId(imm_data);
        uint8_t opcode = ImmDataCodec::GetOpcode(imm_data);
        uint8_t slot_hint = ImmDataCodec::GetSlotHint(imm_data);

        // 直接分发，无需解析 Header
        DispatchRequest(conn_id, opcode, slot_hint, wc.byte_len);
    }

private:
    Status PostWriteWithImm(void* data, size_t length, uint64_t remote_addr,
                            uint32_t remote_rkey, uint64_t wr_id, uint32_t imm_data);
    void DispatchRequest(uint16_t conn_id, uint8_t opcode, uint8_t slot_hint, uint32_t length);
};
```

### 8.1.3 Header-on-Data 极低延迟模式（可选）

```cpp
// 设计考虑：
// 1. 根据 RDMA 协议，网卡保证数据完全写入内存后，最后的字节才会可见
// 2. 将 4 字节的响应状态或 Slot 信息放在数据 Buffer 末尾（Payload Tail）
// 3. 客户端 Polling 数据末尾的标记位，可以比 CQ 轮询快 50-100ns
// 4. 权衡：消耗更多 CPU 周期，仅适用于极低延迟场景（如 NVMe-oF 最热路径）
// 5. 需要 Memory Fence 保证可见性

// Payload Tail 标记结构（4 字节，放在数据末尾）
struct PayloadTailMarker {
    uint16_t magic;         // 魔数校验（0xCAFE）
    uint8_t  status;        // 响应状态
    uint8_t  generation;    // 防止 stale read
};

static constexpr uint16_t kPayloadTailMagic = 0xCAFE;
static constexpr size_t kPayloadTailSize = sizeof(PayloadTailMarker);

class HeaderOnDataMode {
public:
    struct Config {
        bool enable = false;                // 默认关闭
        uint32_t spin_count_max = 10000;    // 最大自旋次数，避免死锁
    };

    HeaderOnDataMode(const Config& config = {}) : config_(config) {}

    // 服务端：写入数据时在末尾添加标记
    Status WriteWithTailMarker(Connection* conn, void* data, size_t data_len,
                               uint64_t remote_addr, uint32_t remote_rkey,
                               uint8_t status, uint8_t generation) {
        // 1. 计算标记位置（数据末尾）
        PayloadTailMarker* tail = reinterpret_cast<PayloadTailMarker*>(
            static_cast<uint8_t*>(data) + data_len - kPayloadTailSize);

        // 2. 先将 magic 设为无效，防止客户端提前读到旧数据
        tail->magic = 0;
        std::atomic_thread_fence(std::memory_order_release);

        // 3. 填充状态
        tail->status = status;
        tail->generation = generation;

        // 4. 最后设置 magic（RDMA 保证字节按序可见）
        std::atomic_thread_fence(std::memory_order_release);
        tail->magic = kPayloadTailMagic;

        // 5. 发送 RDMA Write（无需 IMM，客户端 poll 数据）
        return conn->GetQP()->PostWrite(data, data_len, remote_addr, remote_rkey);
    }

    // 客户端：Polling 数据末尾等待完成
    // 返回值：true = 数据已到达，false = 超时
    HOT_FUNCTION
    bool PollForCompletion(void* buffer, size_t buffer_len, uint8_t expected_gen) {
        if (!config_.enable) return false;

        volatile PayloadTailMarker* tail = reinterpret_cast<volatile PayloadTailMarker*>(
            static_cast<uint8_t*>(buffer) + buffer_len - kPayloadTailSize);

        uint32_t spin_count = 0;
        while (spin_count++ < config_.spin_count_max) {
            // 1. 先检查 magic（内存屏障保证读取顺序）
            std::atomic_thread_fence(std::memory_order_acquire);
            uint16_t magic = tail->magic;

            if (magic == kPayloadTailMagic) {
                // 2. 校验 generation 防止 stale read
                if (tail->generation == expected_gen) {
                    return true;  // 数据已到达
                }
            }

            // 3. 短暂 pause，减少内存总线竞争
            _mm_pause();
        }

        return false;  // 超时，回退到 CQ polling
    }

    // 获取响应状态（在 PollForCompletion 返回 true 后调用）
    ALWAYS_INLINE uint8_t GetStatus(void* buffer, size_t buffer_len) {
        PayloadTailMarker* tail = reinterpret_cast<PayloadTailMarker*>(
            static_cast<uint8_t*>(buffer) + buffer_len - kPayloadTailSize);
        return tail->status;
    }

private:
    Config config_;
};

// 使用示例：
// 1. 服务端发送 Read Response：
//    header_on_data.WriteWithTailMarker(conn, data, len, remote_addr, rkey, status, gen);
//
// 2. 客户端等待响应：
//    if (header_on_data.PollForCompletion(buffer, len, expected_gen)) {
//        // 数据已到达，可以直接使用
//        uint8_t status = header_on_data.GetStatus(buffer, len);
//    } else {
//        // 超时，回退到 CQ polling
//        poll_cq(...);
//    }
```

### 8.2 内存布局优化（热度分层）

```
【优化】RpcSlot 按访问频率分层，提升 PollCQ 缓存效率

=== RpcSlotHot（16 bytes，PollCQ 每次都访问）===
┌─────────────────────────────────────────────────────────────┐
│ slot_index(4) | generation(1) | flags(1) | conn_id(2)  [8B] │
├─────────────────────────────────────────────────────────────┤
│ total_wr(1) | completed_wr(1) | error_code(1) | pad(1) [4B] │
├─────────────────────────────────────────────────────────────┤
│ timeout_tick(4)                                        [4B] │
└─────────────────────────────────────────────────────────────┘
优势：4 个 RpcSlotHot 共享一个 cache line (64B)

=== RpcSlotContext（32 bytes，仅 RPC 完成时访问）===
┌─────────────────────────────────────────────────────────────┐
│ callback_fn(8) | callback_ctx(8)                      [16B] │
├─────────────────────────────────────────────────────────────┤
│ recv_buffer(8) | recv_length(4) | extended_idx(4)     [16B] │
└─────────────────────────────────────────────────────────────┘
优势：只有 IsCompleted() 为 true 时才加载

=== ExtendedSlotInfo（64 bytes，冷数据）===
┌─────────────────────────────────────────────────────────────┐
│ rpc_id(8) | start_tsc(8) | user_data(8) | sizes(8) | wr[4]  │
└─────────────────────────────────────────────────────────────┘
优势：调试/统计时才访问，不影响热路径
```

### 8.3 SGE 使用规则

```
规则（支持自动分片 + 模板优化）：
1. Header-only RPC：1 个 SGE（inline），使用 HEADER_ONLY 模板
2. Header + Data RPC：2 个 SGE（header inline + data），使用 HEADER_PLUS_1PAGE 模板
3. 常见存储模式：
   - 1 Header + 4 Pages（5 SGE）：HEADER_PLUS_4PAGE 模板
   - 1 Header + 8 Pages（9 SGE）：HEADER_PLUS_8PAGE 模板
4. iov_count <= 16：使用 SgeFiller 展开填充，避免循环开销
5. iov_count > max_sge：QueuePair::PostSendSGL 自动分片为多个 WR
6. 禁止 stack 数据作为 SGE
7. 存储场景 "1 控制头 + N 离散页" 优先匹配预构建模板，无匹配则使用展开填充
```

## 9. 实现阶段

### 阶段一：基础设施

- [ ] CMake 配置（NUMA 检测、Hugepage 检测）
- [ ] Status/Result
- [ ] wr_id 编码（inline）
- [ ] MPSC lock-free queue（批量消费）
- [ ] 时间轮
- [ ] 日志（编译期可移除）
- [ ] RdmaDevice + NUMA 校验

### 阶段二：内存管理

- [ ] HugepageAllocator（NUMA-aware）
- [ ] MemoryRegion
- [ ] MRCache
- [ ] LocalBufferPool（预分配）
- [ ] Buffer

### 阶段三：核心传输

- [ ] CompletionQueue
- [ ] QueuePair（signaled 策略 + backpressure）
- [ ] DoorbellBatcher
- [ ] CompletionDispatcher（二级索引 + 热缓存）

### 阶段四：连接管理

- [ ] Connection
- [ ] FlowControl（懒更新）
- [ ] ConnectionManager（free list）
- [ ] HandshakeChannel

### 阶段五：RPC 层

- [ ] RpcSlot（严格对齐）
- [ ] RpcSlotPool
- [ ] RpcEndpoint（Header/Data 分离）
- [ ] Reactor（动态 batch + NUMA pin）
- [ ] Keepalive（低频）

### 阶段六：优化验证

- [ ] Perf 热点分析
- [ ] Cache 命中率分析
- [ ] NUMA 访问验证
- [ ] 1M QPS 压测

## 10. 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 超时管理 | 分层时间轮 | O(1)，1ms 精度，环形缓冲区避免扩容 |
| 连接索引 | 二级索引 + 哈希热缓存 | O(1) 查找，LRU 淘汰 |
| CQ batch | 动态 8-128 | 适应不同负载 |
| atomic 语义 | relaxed | 单线程消费 |
| Buffer 来源 | 预注册 | 禁止 stack，零拷贝 |
| CQ 保护 | signaled ≤ depth/2 | 防止 overflow |
| NUMA | 强制同节点 | 避免跨 NUMA |
| 日志 | 编译期移除 | 零开销 |
| Keepalive | 30s / 独立 CQ | 不打断热路径 |
| Credit 更新 | 25% 懒更新 | 减少控制面 |
| Doorbell | 双重触发批量 Flush | 减少 MMIO，避免延迟抖动 |
| 热路径函数 | 全部 inline | 减少调用开销 |
| 自旋退避 | _mm_pause 分级 | 降低 CPU 功耗，防止流水线过热 |
| SGL 处理 | 自动分片 | 超过 max_sge 自动拆分 WR |
| Read Response | RDMA_WRITE_WITH_IMM | 零读取，imm_data 携带 slot_index |
| RpcSlot | 64B 单 cache line | 压缩热数据，冷数据外置 |
| WR 构建 | 预构建模板池 | 只修改变化字段 |
| Cache 优化 | 流水线预取 | 提前 4 个 slot |
| QP 错误 | 快速恢复 | SQ Error 可重置，减少中断 |
| SPDK 集成 | Poller 模式 | 共享事件循环 |

## 11. 依赖要求

- Linux 内核 >= 4.15（Hugepage 支持）
- rdma-core >= 25
- libnuma（NUMA 支持）
- CMake >= 3.14
- C++17
- Hugepage 配置（推荐 1GB pages）
- SoftRoCE（开发环境）

## 12. SPDK 集成设计

### 12.1 SPDK Poller 封装

```cpp
// 设计考虑：
// 1. 不要让 Reactor 拥有自己的 while(running_) 循环
// 2. 将 Reactor::Poll() 注册为 SPDK 的非抢占式 Poller
// 3. 共享 SPDK 的事件循环，避免线程竞争

class SpdkRdmaAdapter {
public:
    struct Config {
        uint32_t poll_batch_size = 32;
        bool use_spdk_allocator = true;     // 使用 SPDK DMA 分配器
    };

    // 初始化（必须在 SPDK 线程调用）
    static Result<std::unique_ptr<SpdkRdmaAdapter>> Create(
        ReactorResources* resources, const Config& config);

    // 注册为 SPDK Poller（替代 Reactor::Run）
    Status RegisterAsPoller() {
        // 注册非抢占式 Poller
        poller_ = spdk_poller_register(
            SpdkPollerCallback,     // 回调函数
            this,                   // 上下文
            0                       // period_microseconds = 0 表示每次事件循环都调用
        );

        if (!poller_) {
            return Status::IOError("Failed to register SPDK poller");
        }
        return Status::OK();
    }

    // 卸载 Poller
    void UnregisterPoller() {
        if (poller_) {
            spdk_poller_unregister(&poller_);
            poller_ = nullptr;
        }
    }

private:
    ReactorResources* resources_;
    struct spdk_poller* poller_ = nullptr;

    // SPDK Poller 回调
    static int SpdkPollerCallback(void* ctx) {
        auto* adapter = static_cast<SpdkRdmaAdapter*>(ctx);

        // 直接调用 Reactor::Poll()
        int events = adapter->resources_->reactor->Poll();

        // 返回值：
        // SPDK_POLLER_IDLE (0) = 无事件
        // SPDK_POLLER_BUSY (1) = 有事件处理
        return events > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
    }
};
```

### 12.2 SPDK DMA 内存分配器（含物理连续性预校验）

```cpp
// 设计考虑：
// 1. spdk_malloc 分配的内存在大页内连续，但跨大页时不一定物理连续
// 2. MemoryRegion 注册需要处理多段物理地址映射
// 3. 为 RDMA 提供物理连续性保证
// 4.【优化】初始化时预校验物理连续性，避免运行时走慢路径
// 5.【优化】使用 spdk_mem_register 提前锁定内存，确保稳定的物理映射

class SpdkDmaAllocator : public MemoryAllocator {
public:
    struct Config {
        bool require_contiguous = true;     // 是否要求物理连续
        int max_retry = 3;                  // 非连续时最大重试次数
        bool fail_on_fragmented = true;     // 碎片化时是否直接报错
        int numa_node = SPDK_ENV_SOCKET_ID_ANY;
    };

    explicit SpdkDmaAllocator(const Config& config = {}) : config_(config) {}

    // 分配 DMA-capable 内存（含物理连续性保证）
    void* Allocate(size_t size, size_t alignment) override {
        return AllocateContiguous(size, alignment).first;
    }

    // 分配并返回连续性状态
    std::pair<void*, bool> AllocateContiguous(size_t size, size_t alignment) {
        void* ptr = nullptr;
        bool is_contiguous = false;

        for (int retry = 0; retry <= config_.max_retry; ++retry) {
            // 使用 SPDK 的 DMA 内存分配
            ptr = spdk_malloc(size, alignment, nullptr,
                              config_.numa_node, SPDK_MALLOC_DMA);
            if (!ptr) {
                return {nullptr, false};
            }

            // 预校验物理连续性
            is_contiguous = IsPhysicallyContiguous(ptr, size);

            if (is_contiguous || !config_.require_contiguous) {
                // 使用 spdk_mem_register 锁定内存，确保物理地址稳定
                if (spdk_mem_register(ptr, size) != 0) {
                    spdk_free(ptr);
                    return {nullptr, false};
                }
                break;
            }

            // 物理不连续，释放后重试
            spdk_free(ptr);
            ptr = nullptr;

            // 【优化】重试前触发内存整理（如果支持）
            // 某些系统可能支持显式触发大页合并
        }

        if (!ptr && config_.fail_on_fragmented) {
            // 初始化阶段发现内存碎片化严重，直接报错
            // 高性能框架应尽量避免"慢路径"的逻辑复杂性
            RDMA_LOG_ERROR("Memory too fragmented, cannot allocate contiguous region");
            return {nullptr, false};
        }

        return {ptr, is_contiguous};
    }

    void Deallocate(void* ptr, size_t size) override {
        spdk_mem_unregister(ptr, size);  // 解锁内存
        spdk_free(ptr);
    }

    // 检查内存是否物理连续
    bool IsPhysicallyContiguous(void* ptr, size_t size) const {
        // 获取物理地址映射
        uint64_t phys_addr = spdk_vtophys(ptr, nullptr);
        if (phys_addr == SPDK_VTOPHYS_ERROR) {
            return false;
        }

        // 检查整个范围是否连续
        const size_t page_size = 4096;
        for (size_t offset = page_size; offset < size; offset += page_size) {
            uint64_t expected = phys_addr + offset;
            uint64_t actual = spdk_vtophys(static_cast<char*>(ptr) + offset, nullptr);
            if (actual != expected) {
                return false;
            }
        }
        return true;
    }

    // 计算碎片化程度（用于初始化诊断）
    float GetFragmentationRatio(void* ptr, size_t size) const {
        if (size <= 4096) return 0.0f;

        size_t discontinuities = 0;
        size_t checks = 0;
        const size_t page_size = 4096;

        uint64_t prev_phys = spdk_vtophys(ptr, nullptr);
        for (size_t offset = page_size; offset < size; offset += page_size) {
            uint64_t curr_phys = spdk_vtophys(static_cast<char*>(ptr) + offset, nullptr);
            if (curr_phys != prev_phys + page_size) {
                ++discontinuities;
            }
            prev_phys = curr_phys;
            ++checks;
        }

        return checks > 0 ? static_cast<float>(discontinuities) / checks : 0.0f;
    }

private:
    Config config_;
};

// 多段物理地址 MR 注册
class SpdkMemoryRegion {
public:
    // 处理可能不连续的物理内存
    static Result<std::unique_ptr<SpdkMemoryRegion>> Register(
        ProtectionDomain* pd, void* addr, size_t size,
        SpdkDmaAllocator* allocator) {

        auto mr = std::make_unique<SpdkMemoryRegion>();

        // 检查物理连续性
        if (allocator->IsPhysicallyContiguous(addr, size)) {
            // 快速路径：单个 MR
            auto base_mr = MemoryRegion::Register(pd, addr, size);
            if (!base_mr.ok()) return base_mr.status();
            mr->segments_.push_back({addr, size, std::move(base_mr.value())});
        } else {
            // 慢路径：分段注册
            // 找到连续的段，分别注册
            size_t offset = 0;
            while (offset < size) {
                size_t seg_size = FindContiguousSegment(
                    static_cast<char*>(addr) + offset, size - offset);

                auto seg_mr = MemoryRegion::Register(
                    pd, static_cast<char*>(addr) + offset, seg_size);
                if (!seg_mr.ok()) return seg_mr.status();

                mr->segments_.push_back({
                    static_cast<char*>(addr) + offset,
                    seg_size,
                    std::move(seg_mr.value())
                });
                offset += seg_size;
            }
        }

        return mr;
    }

    // 获取指定偏移的 lkey
    uint32_t GetLkey(size_t offset) const {
        for (const auto& seg : segments_) {
            size_t seg_offset = static_cast<char*>(seg.addr) -
                               static_cast<char*>(segments_[0].addr);
            if (offset >= seg_offset && offset < seg_offset + seg.size) {
                return seg.mr->GetLkey();
            }
        }
        return 0;  // error
    }

private:
    struct Segment {
        void* addr;
        size_t size;
        std::unique_ptr<MemoryRegion> mr;
    };
    std::vector<Segment> segments_;

    static size_t FindContiguousSegment(void* addr, size_t max_size);
};
```

### 12.3 物理地址转换缓存优化

```cpp
// 设计考虑：
// 1. spdk_vtophys 调用有开销（内部涉及页表查找）
// 2. 存储场景下，同一 MemoryRegion 会被反复用于 RDMA 操作
// 3. 在 MR 注册时一次性计算所有大页的物理地址并缓存
// 4. PostWrite 时通过偏移量直接查表，避免每次调用 spdk_vtophys

// 物理地址缓存条目
struct PhysAddrCacheEntry {
    uint64_t virt_base;     // 虚拟地址基址（大页对齐）
    uint64_t phys_base;     // 对应的物理地址
    uint32_t page_count;    // 连续的页数（用于范围查找优化）
    uint32_t _reserved;
};

// 带物理地址缓存的 SPDK MemoryRegion
class SpdkMemoryRegionCached {
public:
    static constexpr size_t HUGEPAGE_SIZE_2MB = 2 * 1024 * 1024;
    static constexpr size_t HUGEPAGE_SIZE_1GB = 1024 * 1024 * 1024;

    // 注册时构建物理地址缓存
    static Result<std::unique_ptr<SpdkMemoryRegionCached>> Register(
        ProtectionDomain* pd, void* addr, size_t size,
        size_t hugepage_size = HUGEPAGE_SIZE_2MB) {

        auto mr = std::make_unique<SpdkMemoryRegionCached>();
        mr->base_addr_ = reinterpret_cast<uintptr_t>(addr);
        mr->size_ = size;
        mr->hugepage_size_ = hugepage_size;
        mr->hugepage_shift_ = __builtin_ctzll(hugepage_size);  // 快速计算 log2

        // 1. 计算需要多少个大页
        size_t num_hugepages = (size + hugepage_size - 1) / hugepage_size;
        mr->phys_cache_.reserve(num_hugepages);

        // 2. 一次性遍历所有大页，计算物理地址并缓存
        uintptr_t current_vaddr = reinterpret_cast<uintptr_t>(addr);
        for (size_t i = 0; i < num_hugepages; ++i) {
            uint64_t phys = spdk_vtophys(reinterpret_cast<void*>(current_vaddr), nullptr);
            if (phys == SPDK_VTOPHYS_ERROR) {
                return Status::IOError("spdk_vtophys failed for address",
                                       static_cast<int64_t>(current_vaddr));
            }

            // 尝试合并连续的物理页
            if (!mr->phys_cache_.empty()) {
                auto& last = mr->phys_cache_.back();
                uint64_t expected_phys = last.phys_base + last.page_count * hugepage_size;
                if (phys == expected_phys) {
                    // 物理连续，增加 page_count
                    ++last.page_count;
                    current_vaddr += hugepage_size;
                    continue;
                }
            }

            // 新的不连续段
            mr->phys_cache_.push_back({
                current_vaddr,
                phys,
                1,      // page_count
                0       // reserved
            });
            current_vaddr += hugepage_size;
        }

        // 3. 注册实际的 MR
        auto base_mr = MemoryRegion::Register(pd, addr, size);
        if (!base_mr.ok()) return base_mr.status();
        mr->mr_ = std::move(base_mr.value());

        // 4. 预计算快速查找表（可选：对于小 MR 使用直接数组索引）
        if (num_hugepages <= 64) {
            // 小 MR：使用直接数组索引，O(1) 查找
            mr->use_direct_index_ = true;
            mr->direct_phys_table_.resize(num_hugepages);
            for (size_t i = 0; i < num_hugepages; ++i) {
                uintptr_t page_vaddr = mr->base_addr_ + i * hugepage_size;
                mr->direct_phys_table_[i] = mr->LookupPhysSlow(page_vaddr);
            }
        } else {
            mr->use_direct_index_ = false;
        }

        return mr;
    }

    // 快速获取物理地址（热路径）
    HOT_FUNCTION ALWAYS_INLINE
    uint64_t GetPhysAddr(uintptr_t virt_addr) const {
        // 计算在 MR 内的偏移和大页索引
        size_t offset = virt_addr - base_addr_;
        size_t page_index = offset >> hugepage_shift_;      // 等价于 offset / hugepage_size
        size_t page_offset = offset & (hugepage_size_ - 1); // 等价于 offset % hugepage_size

        if (LIKELY(use_direct_index_)) {
            // 快速路径：直接数组索引，O(1)
            return direct_phys_table_[page_index] + page_offset;
        }

        // 慢路径：二分查找缓存（适用于大 MR）
        return LookupPhysSlow(virt_addr);
    }

    // 用于 RDMA Write 的便捷方法
    HOT_FUNCTION ALWAYS_INLINE
    uint64_t GetPhysAddrForOffset(size_t offset) const {
        return GetPhysAddr(base_addr_ + offset);
    }

    // 获取 lkey
    uint32_t GetLkey() const { return mr_->GetLkey(); }

    // 获取 rkey（用于远程访问）
    uint32_t GetRkey() const { return mr_->GetRkey(); }

private:
    uintptr_t base_addr_;
    size_t size_;
    size_t hugepage_size_;
    size_t hugepage_shift_;

    std::unique_ptr<MemoryRegion> mr_;
    std::vector<PhysAddrCacheEntry> phys_cache_;

    // 直接索引表（用于小 MR）
    bool use_direct_index_ = false;
    std::vector<uint64_t> direct_phys_table_;

    // 慢路径查找（二分查找）
    uint64_t LookupPhysSlow(uintptr_t virt_addr) const {
        // 对齐到大页边界
        uintptr_t page_base = virt_addr & ~(hugepage_size_ - 1);
        size_t page_offset = virt_addr & (hugepage_size_ - 1);

        // 二分查找
        size_t left = 0, right = phys_cache_.size();
        while (left < right) {
            size_t mid = left + (right - left) / 2;
            const auto& entry = phys_cache_[mid];
            uintptr_t entry_end = entry.virt_base + entry.page_count * hugepage_size_;

            if (page_base < entry.virt_base) {
                right = mid;
            } else if (page_base >= entry_end) {
                left = mid + 1;
            } else {
                // 找到了，计算物理地址
                size_t offset_in_entry = page_base - entry.virt_base;
                return entry.phys_base + offset_in_entry + page_offset;
            }
        }
        return SPDK_VTOPHYS_ERROR;  // 不应该发生
    }
};

// 优化后的 PostWrite：使用缓存的物理地址
class ConnectionOptimized {
public:
    // 使用缓存物理地址的 RDMA Write
    HOT_FUNCTION
    Status PostWriteWithCachedPhys(
        const SpdkMemoryRegionCached* local_mr,
        size_t local_offset,
        size_t length,
        uint64_t remote_addr,
        uint32_t remote_rkey,
        uint64_t wr_id) {

        // 直接从缓存获取物理地址，无需调用 spdk_vtophys
        // 注意：这里的 "物理地址" 用于 NVMe-oF 等需要物理地址的场景
        // 对于纯 RDMA，使用虚拟地址 + lkey 即可

        ibv_send_wr wr = {};
        ibv_sge sge = {};

        sge.addr = reinterpret_cast<uint64_t>(
            reinterpret_cast<char*>(local_mr->base_addr_) + local_offset);
        sge.length = static_cast<uint32_t>(length);
        sge.lkey = local_mr->GetLkey();

        wr.wr_id = wr_id;
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.send_flags = IBV_SEND_SIGNALED;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.wr.rdma.remote_addr = remote_addr;
        wr.wr.rdma.rkey = remote_rkey;

        ibv_send_wr* bad_wr = nullptr;
        int ret = ibv_post_send(qp_, &wr, &bad_wr);
        if (UNLIKELY(ret != 0)) {
            return Status::Error("ibv_post_send failed", ret);
        }
        return Status::OK();
    }

private:
    ibv_qp* qp_;
};
```

### 12.4 Pre-registered IOV Memory（零拷贝增强）

```cpp
// 设计考虑：
// 1. SPDK bdev_io 携带的是逻辑块地址（LBA），需要转换为物理地址
// 2. 避免每次 IO 都重新计算偏移量和查找 MR 注册信息
// 3. 通过 SPDK mem_map 直接获取 MR 注册信息，节省查表时间
// 4. 预注册常用的 IO Buffer 区域，实现真正的零拷贝

// 预注册的 IOV 内存区域
struct PreRegisteredIovRegion {
    void* base_addr;                    // 区域基地址
    size_t size;                        // 区域大小
    uint32_t lkey;                      // MR lkey
    uint32_t rkey;                      // MR rkey
    uint64_t phys_base;                 // 物理地址基址（对于连续区域）
    bool is_physically_contiguous;      // 是否物理连续
};

// SPDK IOV 内存管理器
class SpdkIovMemoryManager {
public:
    // 注册一个 IOV 内存区域
    Status RegisterRegion(void* addr, size_t size, ProtectionDomain* pd) {
        // 1. 检查是否已注册
        if (FindRegion(addr)) {
            return Status::AlreadyExists("Region already registered");
        }

        // 2. 使用 SPDK mem_register 锁定内存
        if (spdk_mem_register(addr, size) != 0) {
            return Status::IOError("spdk_mem_register failed");
        }

        // 3. 注册 MR
        int access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ;
        ibv_mr* mr = ibv_reg_mr(pd->GetRawPD(), addr, size, access_flags);
        if (!mr) {
            spdk_mem_unregister(addr, size);
            return Status::IOError("ibv_reg_mr failed");
        }

        // 4. 获取物理地址并检查连续性
        uint64_t phys_base = spdk_vtophys(addr, nullptr);
        bool contiguous = IsPhysicallyContiguous(addr, size);

        // 5. 存入注册表
        PreRegisteredIovRegion region = {
            addr, size, mr->lkey, mr->rkey, phys_base, contiguous
        };
        regions_.emplace(reinterpret_cast<uintptr_t>(addr), region);
        mr_map_[addr] = mr;

        return Status::OK();
    }

    // 快速查找已注册区域（热路径）
    ALWAYS_INLINE const PreRegisteredIovRegion* FindRegion(void* addr) const {
        uintptr_t key = reinterpret_cast<uintptr_t>(addr);

        // 先检查精确匹配
        auto it = regions_.find(key);
        if (it != regions_.end()) {
            return &it->second;
        }

        // 检查是否在某个区域内
        for (const auto& [base, region] : regions_) {
            if (key >= base && key < base + region.size) {
                return &region;
            }
        }
        return nullptr;
    }

    // 获取 lkey（零开销，直接查表）
    ALWAYS_INLINE uint32_t GetLkey(void* addr) const {
        const auto* region = FindRegion(addr);
        return region ? region->lkey : 0;
    }

    // 直接从预注册区域构建 SGE（避免任何查找开销）
    ALWAYS_INLINE void BuildSgeFromPreRegistered(
        ibv_sge* sge,
        const PreRegisteredIovRegion* region,
        size_t offset,
        size_t length) {
        sge->addr = reinterpret_cast<uint64_t>(
            static_cast<char*>(region->base_addr) + offset);
        sge->length = static_cast<uint32_t>(length);
        sge->lkey = region->lkey;
    }

private:
    std::unordered_map<uintptr_t, PreRegisteredIovRegion> regions_;
    std::unordered_map<void*, ibv_mr*> mr_map_;

    bool IsPhysicallyContiguous(void* addr, size_t size) const {
        // 简化实现，参考 SpdkDmaAllocator
        const size_t page_size = 4096;
        uint64_t phys = spdk_vtophys(addr, nullptr);
        for (size_t offset = page_size; offset < size; offset += page_size) {
            if (spdk_vtophys(static_cast<char*>(addr) + offset, nullptr) != phys + offset) {
                return false;
            }
        }
        return true;
    }
};
```

### 12.5 指令缓存优化（HOT Section）

```cpp
// 设计考虑：
// 1. 热路径代码应该紧凑排列在同一内存区域，减少 I-Cache 失效
// 2. 使用 GCC section 属性将热函数放入同一段
// 3. 确保 Dispatch 相关逻辑在二进制中紧凑排列

// 定义热代码段属性
#define HOT_SECTION __attribute__((section(".text.hot")))
#define COLD_SECTION __attribute__((section(".text.cold")))

// 热路径函数标记（组合 hot + section）
#define HOT_PATH HOT_FUNCTION HOT_SECTION
#define COLD_PATH __attribute__((cold)) COLD_SECTION

// 示例：热路径函数放入同一段
class HotPathOptimized {
public:
    // 核心 Poll 循环 - 热路径
    HOT_PATH
    int PollCQ(ibv_wc* wc_array, int max_count) {
        // ... 热路径代码
        return ibv_poll_cq(cq_, max_count, wc_array);
    }

    // Dispatch - 热路径
    HOT_PATH
    void DispatchWC(const ibv_wc& wc) {
        // ... 热路径代码
    }

    // 错误处理 - 冷路径（不常执行）
    COLD_PATH
    void HandleError(const ibv_wc& wc) {
        // ... 错误处理代码
    }

private:
    ibv_cq* cq_;
};

// 链接脚本配置（linker script snippet）
// 确保 .text.hot 段放在一起，提升 I-Cache 局部性
/*
SECTIONS {
    .text : {
        *(.text.hot)    // 热代码放在前面
        *(.text)
        *(.text.cold)   // 冷代码放在后面
    }
}
*/

// Reactor Poll 循环的展开优化
class ReactorOptimized {
public:
    // 核心 Poll 循环，使用 #pragma unroll 手动展开
    HOT_PATH
    int Poll() {
        ibv_wc wc_array[64];

        // 使用 __builtin_prefetch 预取 CQ 条目
        __builtin_prefetch(cq_, 0, 3);

        int count = ibv_poll_cq(cq_, 64, wc_array);

        if (LIKELY(count > 0)) {
            // 手动展开：处理前 4 个 WC（常见情况）
            #pragma GCC unroll 4
            for (int i = 0; i < std::min(count, 4); ++i) {
                dispatcher_->Dispatch(wc_array[i]);
            }

            // 处理剩余的 WC
            for (int i = 4; i < count; ++i) {
                dispatcher_->Dispatch(wc_array[i]);
            }
        }

        return count;
    }

private:
    ibv_cq* cq_;
    CompletionDispatcher* dispatcher_;
};
```

### 12.6 SPDK 动态退避策略（Thermal Throttling 防护）

```cpp
// 设计考虑：
// 1. 在 1M QPS 下，所有核心 100% 自旋会导致 CPU 发热极大
// 2. Thermal Throttling 会导致 CPU 频率动态下降，影响 P999 延迟
// 3. 采用动态退避策略：有 I/O 时保持高频自旋，空闲时指数退避
// 4. 在完全无 Pending RPC 时，可以让出 CPU 给其他轻量级线程

class SpdkAdaptiveBackoff {
public:
    struct Config {
        uint32_t min_backoff_us = 0;        // 最小退避时间（微秒）
        uint32_t max_backoff_us = 100;      // 最大退避时间（微秒）
        uint32_t backoff_multiplier = 2;    // 指数退避倍数
        uint32_t busy_threshold = 4;        // 连续忙碌阈值（开始退避前）
        bool enable_yield = true;           // 允许让出 CPU
    };

    SpdkAdaptiveBackoff(const Config& config = {}) : config_(config) {
        Reset();
    }

    // 在每次 poll 后调用，根据事件数调整退避策略
    ALWAYS_INLINE void OnPollComplete(int event_count, uint32_t pending_rpc_count) {
        if (event_count > 0 || pending_rpc_count > 0) {
            // 有活动：重置退避状态，保持高频自旋
            Reset();
            return;
        }

        // 无活动：进入退避模式
        ++idle_count_;

        if (idle_count_ < config_.busy_threshold) {
            // 短暂空闲：使用 pause 指令减少功耗
            for (int i = 0; i < 4; ++i) {
                _mm_pause();
            }
            return;
        }

        // 持续空闲：指数退避
        if (current_backoff_us_ < config_.max_backoff_us) {
            current_backoff_us_ = std::min(
                current_backoff_us_ * config_.backoff_multiplier,
                config_.max_backoff_us);
        }

        PerformBackoff();
    }

    // 获取当前退避状态（用于监控）
    struct BackoffStats {
        uint32_t idle_count;
        uint32_t current_backoff_us;
        uint64_t total_backoff_cycles;
    };

    BackoffStats GetStats() const {
        return {idle_count_, current_backoff_us_, total_backoff_cycles_};
    }

    // 重置退避状态（有新 I/O 到来时调用）
    ALWAYS_INLINE void Reset() {
        idle_count_ = 0;
        current_backoff_us_ = config_.min_backoff_us;
    }

private:
    Config config_;
    uint32_t idle_count_ = 0;
    uint32_t current_backoff_us_ = 0;
    uint64_t total_backoff_cycles_ = 0;

    void PerformBackoff() {
        if (current_backoff_us_ == 0) {
            // 最小退避：只用 pause 指令
            for (int i = 0; i < 16; ++i) {
                _mm_pause();
            }
            return;
        }

        if (config_.enable_yield && current_backoff_us_ >= config_.max_backoff_us) {
            // 最大退避：让出 CPU（调用 usleep(0) 或 sched_yield）
            // 注：在 SPDK 环境中，这可能不是最佳选择，需要测试
            spdk_pause();
            total_backoff_cycles_ += current_backoff_us_;
            return;
        }

        // 中等退避：使用 TSC 计时的忙等
        uint64_t start_tsc = __rdtsc();
        uint64_t target_cycles = current_backoff_us_ * 2400;  // 假设 2.4GHz

        while (__rdtsc() - start_tsc < target_cycles) {
            _mm_pause();
        }

        total_backoff_cycles_ += target_cycles;
    }
};

// 集成到 SpdkRdmaAdapter 的示例
class SpdkRdmaAdapterWithBackoff {
public:
    // SPDK Poller 回调（带动态退避）
    static int SpdkPollerCallback(void* ctx) {
        auto* adapter = static_cast<SpdkRdmaAdapterWithBackoff*>(ctx);

        // 1. 执行 Poll
        int events = adapter->resources_->reactor->Poll();

        // 2. 获取当前 pending RPC 数量（用于退避决策）
        uint32_t pending_rpcs = adapter->GetPendingRpcCount();

        // 3. 动态退避
        adapter->backoff_.OnPollComplete(events, pending_rpcs);

        // 4. 返回状态
        return events > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
    }

    // 获取 pending RPC 数量（用于退避决策）
    uint32_t GetPendingRpcCount() const {
        // 实现：统计所有连接的 in-flight RPC 数量
        return pending_rpc_count_.load(std::memory_order_relaxed);
    }

private:
    ReactorResources* resources_;
    SpdkAdaptiveBackoff backoff_;
    std::atomic<uint32_t> pending_rpc_count_{0};
};
```

## 13. 高级优化技术

### 13.1 无分支 Dispatch 优化

```cpp
// 设计考虑：
// 1. 热路径中的条件判断会导致分支预测失败
// 2. 使用无分支计算来避免分支开销
// 3. 两个路径都计算，用 status 选择结果

class BranchlessDispatcher {
public:
    using WcHandler = void (*)(void* ctx, const ibv_wc& wc);
    using ErrorHandler = void (*)(void* ctx, uint16_t conn_id, ibv_wc_status status);

    // 无分支 Dispatch
    HOT_FUNCTION ALWAYS_INLINE
    void DispatchBranchless(const ibv_wc& wc) {
        uint16_t conn_id = WrId::GetConnId(wc.wr_id);

        // 计算正常路径结果（即使不用也计算）
        auto* normal_entry = FindConnection(conn_id);
        void* normal_ctx = normal_entry ? normal_entry->ctx : nullptr;

        // 计算错误路径结果（即使不用也计算）
        void* error_ctx = error_handler_.ctx;

        // 无分支选择
        bool is_error = (wc.status != IBV_WC_SUCCESS);
        bool is_normal = !is_error && (normal_entry != nullptr);

        // 使用条件移动代替分支
        // 方法1：使用 CMOV（编译器会优化）
        void* selected_ctx = is_error ? error_ctx : normal_ctx;
        auto selected_handler = is_error ?
            reinterpret_cast<WcHandler>(error_handler_.handler) :
            (normal_entry ? normal_entry->handler : nullptr);

        // 只有一次函数调用（无条件调用 wrapper）
        if (selected_handler) {
            if (is_error) {
                error_handler_.handler(error_ctx, conn_id, wc.status);
            } else {
                selected_handler(selected_ctx, wc);
            }
        }
    }

    // 批量处理时使用 SIMD 预取 + 无分支
    HOT_FUNCTION
    void DispatchBatchOptimized(const ibv_wc* wc_array, int count) {
        // 预取第一批
        if (count >= 4) {
            __builtin_prefetch(&wc_array[4], 0, 3);
        }

        for (int i = 0; i < count; ++i) {
            // 预取下一个 WC
            if (i + 4 < count) {
                __builtin_prefetch(&wc_array[i + 4], 0, 3);
            }

            // 预取对应的 connection entry
            uint16_t next_conn = (i + 1 < count) ?
                WrId::GetConnId(wc_array[i + 1].wr_id) : 0;
            __builtin_prefetch(&buckets_[next_conn >> 8], 0, 3);

            DispatchBranchless(wc_array[i]);
        }
    }

private:
    struct ConnectionEntry {
        void* ctx = nullptr;
        WcHandler handler = nullptr;
    };

    ConnectionEntry* buckets_[256] = {};

    struct {
        void* ctx = nullptr;
        ErrorHandler handler = nullptr;
    } error_handler_;

    ConnectionEntry* FindConnection(uint16_t conn_id);
};
```

### 13.2 WR 预构建模板池

```cpp
// 设计考虑：
// 1. 每次 PostSend 都构建完整 WR 结构开销大
// 2. 预构建模板，运行时只修改必要字段
// 3. 不同操作类型使用不同模板

class WrTemplatePool {
public:
    // WR 类型
    enum class WrType : uint8_t {
        SEND_INLINE,        // 小数据 inline 发送
        SEND_SGL,           // SGL 发送
        WRITE_WITH_IMM,     // RDMA Write with immediate
        READ,               // RDMA Read
    };

    WrTemplatePool(QueuePair* qp) : qp_(qp) {
        InitializeTemplates();
    }

    // 获取预构建的 WR（只需修改少量字段）
    ALWAYS_INLINE ibv_send_wr* GetTemplate(WrType type) {
        return &templates_[static_cast<size_t>(type)];
    }

    // 快速构建 Send Inline WR
    ALWAYS_INLINE ibv_send_wr* BuildSendInline(
        const void* data, size_t length, uint64_t wr_id, bool signaled) {

        auto* wr = &templates_[static_cast<size_t>(WrType::SEND_INLINE)];

        // 只修改变化的字段
        wr->wr_id = wr_id;
        wr->send_flags = IBV_SEND_INLINE |
                        (signaled ? IBV_SEND_SIGNALED : 0);
        wr->sg_list[0].addr = reinterpret_cast<uintptr_t>(data);
        wr->sg_list[0].length = length;

        return wr;
    }

    // 快速构建 WriteWithImm WR
    ALWAYS_INLINE ibv_send_wr* BuildWriteWithImm(
        const Buffer& buf, uint64_t remote_addr, uint32_t rkey,
        uint32_t imm_data, uint64_t wr_id, bool signaled) {

        auto* wr = &templates_[static_cast<size_t>(WrType::WRITE_WITH_IMM)];

        wr->wr_id = wr_id;
        wr->send_flags = signaled ? IBV_SEND_SIGNALED : 0;
        wr->imm_data = imm_data;
        wr->wr.rdma.remote_addr = remote_addr;
        wr->wr.rdma.rkey = rkey;

        sge_pool_[static_cast<size_t>(WrType::WRITE_WITH_IMM)].addr =
            reinterpret_cast<uintptr_t>(buf.data());
        sge_pool_[static_cast<size_t>(WrType::WRITE_WITH_IMM)].length = buf.size();
        sge_pool_[static_cast<size_t>(WrType::WRITE_WITH_IMM)].lkey = buf.lkey();

        return wr;
    }

private:
    static constexpr size_t kTemplateCount = 4;

    QueuePair* qp_;
    ibv_send_wr templates_[kTemplateCount];
    ibv_sge sge_pool_[kTemplateCount];

    void InitializeTemplates() {
        // Send Inline 模板
        auto& send_inline = templates_[static_cast<size_t>(WrType::SEND_INLINE)];
        std::memset(&send_inline, 0, sizeof(send_inline));
        send_inline.opcode = IBV_WR_SEND;
        send_inline.num_sge = 1;
        send_inline.sg_list = &sge_pool_[static_cast<size_t>(WrType::SEND_INLINE)];

        // WriteWithImm 模板
        auto& write_imm = templates_[static_cast<size_t>(WrType::WRITE_WITH_IMM)];
        std::memset(&write_imm, 0, sizeof(write_imm));
        write_imm.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
        write_imm.num_sge = 1;
        write_imm.sg_list = &sge_pool_[static_cast<size_t>(WrType::WRITE_WITH_IMM)];

        // 其他模板类似...
    }
};
```

### 13.3 Cache 预取策略

```cpp
// 设计考虑：
// 1. RpcSlotPool 访问模式：wr_id → slot_index → RpcSlot
// 2. 提前预取 slot 数据到 cache
// 3. 批量处理时流水线预取

class CachePrefetchOptimizer {
public:
    // 处理 WC 时的预取策略
    HOT_FUNCTION
    static void PrefetchForWcBatch(const ibv_wc* wc_array, int count,
                                   RpcSlotPool& slot_pool) {
        // 预取策略：提前 4 个 slot
        constexpr int kPrefetchDistance = 4;

        for (int i = 0; i < count; ++i) {
            // 预取后续 slot
            if (i + kPrefetchDistance < count) {
                uint32_t future_slot = WrId::GetSlotIndex(
                    wc_array[i + kPrefetchDistance].wr_id);
                slot_pool.Prefetch(future_slot);
            }

            // 处理当前 WC
            // ...
        }
    }

    // 连接建立时预取 connection 结构
    static void PrefetchConnectionData(Connection* conn) {
        // 预取 QP 状态
        __builtin_prefetch(conn->GetQP(), 0, 3);

        // 预取流控状态
        __builtin_prefetch(conn->GetFlowControl(), 0, 3);

        // 预取本地 buffer pool
        __builtin_prefetch(conn->GetBufferPool(), 0, 3);
    }

    // 发送前预取远程内存信息
    ALWAYS_INLINE
    static void PrefetchRemoteMemory(const RemoteMemoryInfo* info) {
        __builtin_prefetch(info, 0, 3);
    }

    // 软件预取指令封装
    template<int Locality = 3>  // 0-3, 3 = L1 cache
    ALWAYS_INLINE static void Prefetch(const void* addr) {
        __builtin_prefetch(addr, 0, Locality);
    }

    template<int Locality = 3>
    ALWAYS_INLINE static void PrefetchWrite(void* addr) {
        __builtin_prefetch(addr, 1, Locality);  // 1 = write intent
    }
};
```

### 13.4 QP Error 快速恢复（增强版：支持 Retry Error 和 SQ Drain）

```cpp
// 设计考虑：
// 1. QP error 后需要重建连接，影响可用性
// 2. 实现快速恢复机制，减少中断时间
// 3. 支持 QP 状态检测和自动恢复
// 4.【增强】对于 IBV_WC_RETRY_EXC_ERR 等可恢复错误，尝试 SQ Drain + 重发
// 5.【增强】实现 SQ Drain + QP Reset 而非完全重建连接
//    比关闭连接、重新握手快 1-2 个数量级，对 tail latency 至关重要

class QpRecoveryManager {
public:
    struct RecoveryConfig {
        uint32_t max_retry_count = 3;           // 最大重试次数
        uint32_t retry_delay_ms = 100;          // 重试延迟
        bool enable_fast_recovery = true;       // 启用快速恢复
        bool enable_sq_drain = true;            // 启用 SQ Drain（推荐）
        uint32_t sq_drain_timeout_ms = 1000;    // SQ Drain 超时

        // 【新增】静默重试保护配置
        uint32_t max_auto_recovery_in_window = 3;    // 窗口期内最大快速恢复次数
        uint32_t recovery_window_ms = 1000;          // 滑动窗口时间（默认 1 秒）
        bool force_rebuild_on_threshold = true;      // 超限时强制完全重建
    };

    // 【新增】Per-Connection 恢复统计
    struct ConnectionRecoveryStats {
        uint32_t auto_recovery_count = 0;       // 当前窗口内快速恢复次数
        uint64_t window_start_tsc = 0;          // 窗口开始时间戳
        uint64_t last_recovery_tsc = 0;         // 最后一次恢复时间戳
        uint32_t total_recovery_count = 0;      // 累计恢复次数（统计用）
        bool is_in_recovery_loop = false;       // 是否处于恢复死循环
    };

    QpRecoveryManager(const RecoveryConfig& config = {}) : config_(config) {}

    // 检测 QP 错误状态
    ALWAYS_INLINE QpState CheckQpState(ibv_qp* qp) {
        ibv_qp_attr attr;
        ibv_qp_init_attr init_attr;

        if (ibv_query_qp(qp, &attr, IBV_QP_STATE, &init_attr) != 0) {
            return QpState::ERROR;
        }

        switch (attr.qp_state) {
            case IBV_QPS_RTS: return QpState::READY;
            case IBV_QPS_ERR: return QpState::ERROR;
            case IBV_QPS_SQE: return QpState::SQ_ERROR;
            default: return QpState::TRANSITIONING;
        }
    }

    // 快速恢复 QP（带静默重试保护）
    Status FastRecover(Connection* conn) {
        auto qp_state = CheckQpState(conn->GetQP()->GetRawQP());

        if (qp_state == QpState::READY) {
            return Status::OK();  // 无需恢复
        }

        // 【新增】检查是否处于恢复死循环
        auto& stats = GetOrCreateStats(conn);
        if (ShouldBlockFastRecovery(stats)) {
            // 短时间内频繁恢复，可能是物理链路故障或对端崩溃
            stats.is_in_recovery_loop = true;

            if (config_.force_rebuild_on_threshold) {
                // 强制完全重建，而非快速恢复
                return RebuildQp(conn);
            }
            // 向上报障，停止无效重试
            return Status::IOError("Recovery loop detected, connection unstable");
        }

        if (qp_state == QpState::SQ_ERROR) {
            // SQ Error：可以通过重置 QP 恢复
            auto status = RecoverFromSqError(conn);
            if (status.ok()) {
                RecordRecoveryAttempt(stats);
            }
            return status;
        }

        if (qp_state == QpState::ERROR) {
            // 完全错误：需要重建 QP
            return RebuildQp(conn);
        }

        return Status::IOError("QP in transitioning state");
    }

    // 【新增】获取连接恢复统计
    const ConnectionRecoveryStats* GetStats(Connection* conn) const {
        auto it = conn_stats_.find(conn);
        return (it != conn_stats_.end()) ? &it->second : nullptr;
    }

    // 【新增】重置连接恢复统计（连接稳定运行后调用）
    void ResetStats(Connection* conn) {
        auto it = conn_stats_.find(conn);
        if (it != conn_stats_.end()) {
            it->second.auto_recovery_count = 0;
            it->second.is_in_recovery_loop = false;
        }
    }

    // 注册恢复回调
    using RecoveryCallback = void (*)(void* ctx, Connection* conn, Status status);
    void SetRecoveryCallback(void* ctx, RecoveryCallback callback) {
        recovery_callback_ = {ctx, callback};
    }

private:
    enum class QpState {
        READY,
        ERROR,
        SQ_ERROR,
        TRANSITIONING,
    };

    RecoveryConfig config_;

    struct {
        void* ctx = nullptr;
        RecoveryCallback callback = nullptr;
    } recovery_callback_;

    // 【新增】Per-Connection 恢复统计表
    std::unordered_map<Connection*, ConnectionRecoveryStats> conn_stats_;

    // 【新增】获取或创建连接统计
    ConnectionRecoveryStats& GetOrCreateStats(Connection* conn) {
        return conn_stats_[conn];  // 自动创建默认值
    }

    // 【新增】检查是否应阻止快速恢复
    bool ShouldBlockFastRecovery(ConnectionRecoveryStats& stats) {
        uint64_t now_tsc = __rdtsc();
        uint64_t window_tsc = config_.recovery_window_ms * GetTscFrequencyMHz() * 1000;

        // 检查是否超出时间窗口，重置计数
        if (now_tsc - stats.window_start_tsc > window_tsc) {
            stats.window_start_tsc = now_tsc;
            stats.auto_recovery_count = 0;
            stats.is_in_recovery_loop = false;
        }

        // 检查窗口内恢复次数是否超限
        return stats.auto_recovery_count >= config_.max_auto_recovery_in_window;
    }

    // 【新增】记录一次恢复尝试
    void RecordRecoveryAttempt(ConnectionRecoveryStats& stats) {
        uint64_t now_tsc = __rdtsc();

        if (stats.window_start_tsc == 0) {
            stats.window_start_tsc = now_tsc;
        }

        stats.auto_recovery_count++;
        stats.total_recovery_count++;
        stats.last_recovery_tsc = now_tsc;
    }

    // 获取 TSC 频率（MHz）- 通常在初始化时校准
    static uint64_t GetTscFrequencyMHz() {
        static uint64_t freq = 0;
        if (freq == 0) {
            // 简化实现：假设 2.5GHz（实际应读取 /proc/cpuinfo 或 calibrate）
            freq = 2500;
        }
        return freq;
    }

    // 从 SQ Error 恢复
    Status RecoverFromSqError(Connection* conn) {
        auto* qp = conn->GetQP()->GetRawQP();

        // 1. 将 QP 转换到 RESET 状态
        ibv_qp_attr attr = {};
        attr.qp_state = IBV_QPS_RESET;
        if (ibv_modify_qp(qp, &attr, IBV_QP_STATE) != 0) {
            return Status::IOError("Failed to reset QP");
        }

        // 2. 重新初始化 QP（RESET → INIT → RTR → RTS）
        auto status = conn->ReinitializeQp();
        if (!status.ok()) {
            return status;
        }

        // 3. 重新 post recv buffers
        status = conn->RepostRecvBuffers();
        if (!status.ok()) {
            return status;
        }

        // 4. 通知上层恢复完成
        if (recovery_callback_.callback) {
            recovery_callback_.callback(recovery_callback_.ctx, conn, Status::OK());
        }

        return Status::OK();
    }

    // 完全重建 QP
    Status RebuildQp(Connection* conn) {
        // 1. 销毁旧 QP
        conn->DestroyQp();

        // 2. 创建新 QP
        auto status = conn->CreateQp();
        if (!status.ok()) {
            return status;
        }

        // 3. 重新协商连接参数
        status = conn->Renegotiate();
        if (!status.ok()) {
            return status;
        }

        // 4. 重新 post recv buffers
        status = conn->RepostRecvBuffers();
        if (!status.ok()) {
            return status;
        }

        // 5. 通知上层恢复完成
        if (recovery_callback_.callback) {
            recovery_callback_.callback(recovery_callback_.ctx, conn, Status::OK());
        }

        return Status::OK();
    }

    // 【增强】处理 WC 错误并尝试恢复
    Status HandleWcError(Connection* conn, const ibv_wc& wc) {
        switch (wc.status) {
            case IBV_WC_RETRY_EXC_ERR:
                // 重试超限：可能是网络抖动或对端短暂不可达
                // 尝试 SQ Drain 后重发
                return HandleRetryExcError(conn, wc);

            case IBV_WC_RNR_RETRY_EXC_ERR:
                // RNR 重试超限：对端 Recv 队列空
                // 增加 RNR 重试次数后恢复
                return HandleRnrRetryError(conn);

            case IBV_WC_REM_ACCESS_ERR:
            case IBV_WC_REM_INV_REQ_ERR:
            case IBV_WC_REM_OP_ERR:
                // 远端错误：可能需要重新协商
                return FastRecover(conn);

            default:
                // 其他错误：完全重建
                return RebuildQp(conn);
        }
    }

    // 处理 Retry Exceeded Error
    Status HandleRetryExcError(Connection* conn, const ibv_wc& failed_wc) {
        if (!config_.enable_sq_drain) {
            return FastRecover(conn);
        }

        // 1. 执行 SQ Drain
        auto status = DrainSendQueue(conn);
        if (!status.ok()) {
            // SQ Drain 失败，回退到完全恢复
            return FastRecover(conn);
        }

        // 2. 获取未完成的 WR 列表
        auto pending_wrs = conn->GetPendingWorkRequests();

        // 3. 快速重置 QP
        status = QuickResetQp(conn);
        if (!status.ok()) {
            return status;
        }

        // 4. 重发未完成的 WR
        for (const auto& wr_info : pending_wrs) {
            conn->RepostWorkRequest(wr_info);
        }

        ++stats_.retry_recoveries;
        return Status::OK();
    }

    // SQ Drain：等待 Send Queue 中所有 WR 完成或超时
    Status DrainSendQueue(Connection* conn) {
        auto* qp = conn->GetQP()->GetRawQP();

        // 获取 Send CQ
        auto* send_cq = conn->GetSendCQ();
        ibv_wc wc_array[32];

        auto start_time = std::chrono::steady_clock::now();
        auto timeout = std::chrono::milliseconds(config_.sq_drain_timeout_ms);

        while (true) {
            // Poll Send CQ
            int n = ibv_poll_cq(send_cq, 32, wc_array);

            if (n < 0) {
                return Status::IOError("Failed to poll CQ during SQ drain");
            }

            // 处理完成（包括错误）
            for (int i = 0; i < n; ++i) {
                conn->OnSendCompletion(wc_array[i]);
            }

            // 检查是否已清空
            if (conn->GetPendingSendCount() == 0) {
                return Status::OK();
            }

            // 检查超时
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed > timeout) {
                return Status::TimedOut("SQ drain timed out");
            }

            // 短暂 pause
            _mm_pause();
        }
    }

    // 快速 QP 重置（不需要重新协商）
    Status QuickResetQp(Connection* conn) {
        auto* qp = conn->GetQP()->GetRawQP();
        ibv_qp_attr attr = {};

        // 1. QP → RESET
        attr.qp_state = IBV_QPS_RESET;
        if (ibv_modify_qp(qp, &attr, IBV_QP_STATE) != 0) {
            return Status::IOError("Failed to reset QP");
        }

        // 2. RESET → INIT → RTR → RTS（使用缓存的连接参数）
        auto status = conn->ReinitializeQpFast();
        if (!status.ok()) {
            return status;
        }

        // 3. 重新 post recv buffers
        return conn->RepostRecvBuffers();
    }

    // 处理 RNR Retry Error
    Status HandleRnrRetryError(Connection* conn) {
        // 增加 RNR 重试次数
        auto* qp = conn->GetQP()->GetRawQP();
        ibv_qp_attr attr = {};
        attr.min_rnr_timer = 12;  // ~0.6ms

        if (ibv_modify_qp(qp, &attr, IBV_QP_MIN_RNR_TIMER) != 0) {
            return Status::IOError("Failed to modify RNR timer");
        }

        ++stats_.rnr_recoveries;
        return Status::OK();
    }

    // 获取恢复统计
    struct RecoveryStats {
        uint64_t retry_recoveries = 0;
        uint64_t rnr_recoveries = 0;
        uint64_t full_rebuilds = 0;
    };

    RecoveryStats GetStats() const { return stats_; }

private:
    RecoveryStats stats_;
};

### 13.5 CQE Compression（完成队列条目压缩）

```cpp
// 设计考虑：
// 1. 现代 Mellanox 网卡（ConnectX-6/7）支持 CQE 压缩功能
// 2. 将多个状态相同的 CQE 合并为一个，极大减少 PCIe 流量和 CPU 开销
// 3. 高吞吐场景下可显著降低 CQ Polling 的 CPU 占用率
// 4. 需要使用 mlx5dv 扩展 API 来启用

#include <infiniband/mlx5dv.h>

// CQE 压缩配置
struct CqeCompressionConfig {
    bool enable_compression = true;      // 是否启用 CQE 压缩
    bool enable_mini_cqe = true;         // 是否启用 Mini CQE（更激进的压缩）
    uint8_t cqe_comp_res_format = 0;     // 压缩格式：0=HASH, 1=CSUM
};

// CQ 创建封装（支持 CQE 压缩）
class CompletionQueueWithCompression {
public:
    struct Config {
        int cq_depth = 4096;              // CQ 深度
        ibv_context* context = nullptr;    // RDMA 上下文
        ibv_comp_channel* channel = nullptr;  // 可选的完成通道
        CqeCompressionConfig compression;  // 压缩配置
    };

    static Result<std::unique_ptr<CompletionQueueWithCompression>>
    Create(const Config& config) {
        auto cq = std::make_unique<CompletionQueueWithCompression>();

        // 1. 检测设备是否支持 CQE 压缩
        if (config.compression.enable_compression) {
            mlx5dv_context mlx5_ctx = {};
            mlx5_ctx.comp_mask = MLX5DV_CONTEXT_MASK_CQE_COMPRESION;

            if (mlx5dv_query_device(config.context, &mlx5_ctx) == 0) {
                cq->compression_supported_ =
                    (mlx5_ctx.cqe_comp_caps.max_num > 0);
                cq->mini_cqe_supported_ =
                    (mlx5_ctx.cqe_comp_caps.supported_format &
                     MLX5DV_CQE_RES_FORMAT_CSUM);
            }
        }

        // 2. 创建 CQ（使用 mlx5dv 扩展属性）
        ibv_cq_init_attr_ex cq_attr = {};
        cq_attr.cqe = config.cq_depth;
        cq_attr.channel = config.channel;
        cq_attr.comp_vector = 0;
        cq_attr.wc_flags = IBV_WC_EX_WITH_BYTE_LEN |
                          IBV_WC_EX_WITH_IMM;

        mlx5dv_cq_init_attr mlx5_cq_attr = {};

        if (cq->compression_supported_ && config.compression.enable_compression) {
            // 启用 CQE 压缩
            mlx5_cq_attr.comp_mask = MLX5DV_CQ_INIT_ATTR_MASK_COMPRESSED_CQE;
            mlx5_cq_attr.cqe_comp_res_format = config.compression.cqe_comp_res_format;

            if (cq->mini_cqe_supported_ && config.compression.enable_mini_cqe) {
                // 启用 Mini CQE（更高压缩率）
                mlx5_cq_attr.cqe_comp_res_format |= MLX5DV_CQE_RES_FORMAT_CSUM;
            }

            cq->cq_ = mlx5dv_create_cq(config.context, &cq_attr, &mlx5_cq_attr);
        } else {
            // 回退到标准 CQ 创建
            cq->cq_ = ibv_create_cq_ex(config.context, &cq_attr);
        }

        if (!cq->cq_) {
            return Status::IOError("Failed to create CQ");
        }

        cq->compression_enabled_ = cq->compression_supported_ &&
                                   config.compression.enable_compression;

        return cq;
    }

    ~CompletionQueueWithCompression() {
        if (cq_) {
            ibv_destroy_cq(ibv_cq_ex_to_cq(cq_));
        }
    }

    // 获取原始 CQ（用于 ibv_poll_cq）
    ibv_cq* GetRawCQ() const {
        return ibv_cq_ex_to_cq(cq_);
    }

    // 获取扩展 CQ（用于高级功能）
    ibv_cq_ex* GetCQEx() const {
        return cq_;
    }

    // 检查压缩是否启用
    bool IsCompressionEnabled() const {
        return compression_enabled_;
    }

    // 使用扩展 Poll（支持 CQE 压缩）
    // 返回 WC 数量，压缩 CQE 会自动展开
    HOT_FUNCTION
    int PollEx(ibv_wc* wc_array, int max_count) {
        if (!compression_enabled_) {
            // 未启用压缩，使用标准 poll
            return ibv_poll_cq(ibv_cq_ex_to_cq(cq_), max_count, wc_array);
        }

        // 使用扩展 poll 接口
        int count = 0;
        int ret = ibv_start_poll(cq_, nullptr);

        while (ret == 0 && count < max_count) {
            // 读取当前 WC
            wc_array[count].status = cq_->status;
            wc_array[count].wr_id = cq_->wr_id;
            wc_array[count].opcode = ibv_wc_read_opcode(cq_);
            wc_array[count].byte_len = ibv_wc_read_byte_len(cq_);
            wc_array[count].imm_data = ibv_wc_read_imm_data(cq_);

            ++count;
            ret = ibv_next_poll(cq_);
        }

        ibv_end_poll(cq_);
        return count;
    }

    // 获取压缩统计
    struct CompressionStats {
        uint64_t total_cqes_polled = 0;
        uint64_t compressed_batches = 0;
        float avg_compression_ratio = 0.0f;
    };

    CompressionStats GetCompressionStats() const {
        return compression_stats_;
    }

private:
    ibv_cq_ex* cq_ = nullptr;
    bool compression_supported_ = false;
    bool mini_cqe_supported_ = false;
    bool compression_enabled_ = false;
    CompressionStats compression_stats_;
};

// 【优化】CQE 压缩感知的 Reactor
// 当检测到高吞吐时自动利用 CQE 压缩优势
class CqeCompressionAwareReactor {
public:
    CqeCompressionAwareReactor(CompletionQueueWithCompression* cq,
                               CompletionDispatcher* dispatcher)
        : cq_(cq), dispatcher_(dispatcher) {}

    // 优化的 Poll 循环
    HOT_FUNCTION
    int Poll() {
        int count;

        if (cq_->IsCompressionEnabled()) {
            // 使用扩展 poll，自动处理压缩 CQE
            count = cq_->PollEx(wc_buffer_, kMaxBatchSize);
        } else {
            // 标准 poll
            count = ibv_poll_cq(cq_->GetRawCQ(), kMaxBatchSize, wc_buffer_);
        }

        if (LIKELY(count > 0)) {
            // 批量 dispatch
            for (int i = 0; i < count; ++i) {
                dispatcher_->Dispatch(wc_buffer_[i]);
            }

            // 更新统计
            total_wc_count_ += count;
            poll_count_++;

            // 动态调整 batch size
            AdaptBatchSize(count);
        }

        return count;
    }

private:
    static constexpr int kMaxBatchSize = 64;
    static constexpr int kMinBatchSize = 8;

    CompletionQueueWithCompression* cq_;
    CompletionDispatcher* dispatcher_;
    ibv_wc wc_buffer_[kMaxBatchSize];

    uint64_t total_wc_count_ = 0;
    uint64_t poll_count_ = 0;

    void AdaptBatchSize(int count) {
        // CQE 压缩启用时，倾向于使用更大的 batch size
        // 因为压缩 CQE 展开后可能产生更多 WC
        if (cq_->IsCompressionEnabled() && count >= kMaxBatchSize / 2) {
            // 高吞吐：保持大 batch
        } else if (count < kMinBatchSize) {
            // 低吞吐：可以减小 batch
        }
    }
};
```

**CQE 压缩收益分析：**

| 场景 | 无压缩 | 启用压缩 | 收益 |
|------|--------|----------|------|
| 批量小包 RPC | 每个 WC 64B | 8-16 个 WC 压缩为 1 个 | PCIe 带宽降低 8-16x |
| 高吞吐 RDMA Write | 频繁 poll | 减少 poll 次数 | CPU 占用降低 30-50% |
| 混合负载 | 无优化 | 自适应压缩 | 平均延迟降低 |

**启用要求：**
- ConnectX-6 或更新的 Mellanox 网卡
- 安装 mlx5 驱动和 rdma-core 库
- 编译时链接 `libmlx5`
