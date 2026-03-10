# RDMA Framework 设计文档与技术评审

## 1. 项目概述

本项目是一个高性能 RDMA RPC 框架，目标是在 RC (Reliable Connection) 模式下，基于 RoCE v2 实现**单核 1M+ QPS** 的吞吐能力。框架采用 polling 模式进行完成通知，使用 C++17 RAII 管理资源生命周期。

### 1.1 技术栈

| 项目 | 选型 |
|------|------|
| 语言 | C++17 |
| RDMA 传输 | libibverbs + librdmacm |
| 构建系统 | CMake |
| 连接模式 | RC (Reliable Connection) |
| 完成通知 | Polling (非 interrupt) |
| 内存管理 | Hugepage + NUMA-aware |

---

## 2. 整体架构

```
┌───────────────────────────────────────────────────┐
│                  Application Layer                │
│            (KV Store / Custom Service)            │
├───────────────────────────────────────────────────┤
│                    RPC Layer                      │
│   RpcEndpoint · RpcSlotPool · CompletionDispatcher│
├───────────────────────────────────────────────────┤
│                 Transport Layer                   │
│   Connection · ConnectionManager · FlowControl    │
│              TcpHandshakeChannel                  │
├───────────────────────────────────────────────────┤
│                   Core Layer                      │
│   RdmaDevice · ProtectionDomain · MemoryRegion    │
│   CompletionQueue · QueuePair · DoorbellBatcher   │
├───────────────────────────────────────────────────┤
│                 Memory Layer                      │
│   Buffer · LocalBufferPool · HugepageAllocator    │
├───────────────────────────────────────────────────┤
│                 Reactor Layer                     │
│   Reactor (Event Loop) · NUMA Utilities           │
├───────────────────────────────────────────────────┤
│              Common Utilities                     │
│   Status/Result · WrId · MpscQueue · TimingWheel  │
│                  Logging                          │
└───────────────────────────────────────────────────┘
```

### 2.1 线程模型

采用 **single-threaded per Reactor** 模型：

- 所有 RDMA verbs 操作在 Reactor 线程内完成
- Connection / QP / RpcEndpoint 归属于特定 Reactor
- LocalBufferPool 为 per-Reactor，无锁
- 跨线程通信唯一入口：`Reactor::Execute()`（基于 lock-free MPSC 队列）

```
Thread-1 (Reactor-1)          Thread-2 (Reactor-2)
   │                              │
   ├── Poll CQ                    ├── Poll CQ
   ├── Dispatch completions       ├── Dispatch completions
   ├── Run custom pollers         ├── Run custom pollers
   ├── Process MPSC tasks         ├── Process MPSC tasks
   └── Check timeouts             └── Check timeouts

Other threads ──── Execute(task) ──→ MPSC Queue ──→ Reactor thread
```

---

## 3. 各层详细设计

### 3.1 Common 层

#### 3.1.1 错误处理：Status / Result<T>

```cpp
enum ErrorCode: NOT_FOUND, RESOURCE_EXHAUSTED, TIMEOUT, IO_ERROR, ...
Status: 不可变错误对象 (code + message + errno)
Result<T>: 带值的 tagged union（类似 Rust Result<T, E>）
```

**设计评价**：
- 优点：统一的错误传播机制，避免异常开销；`Result<T>` monad 模式使错误处理更安全
- 缺点：相比 `std::expected`（C++23）功能较少，但在 C++17 下是合理选择

#### 3.1.2 WrId 编码

将 64 位 `wr_id` 编码为多个字段：

```
|  conn_id(16)  |  opcode(8)  |  generation(8)  |  slot_index(32)  |
```

**设计评价**：
- 优点：一次 CQE 就能获取所有路由信息，避免额外查找
- 缺点：slot_index 实际只需 20 位（RpcSlotHot 中的定义），32 位有冗余空间

#### 3.1.3 MPSC 队列

Lock-free 多生产者单消费者队列，用于跨线程 task 投递。

**设计评价**：
- 优点：零锁开销，batch PopAll 一次交换整个链表
- 缺点：链表结构导致 cache locality 不佳，高争用下 CAS 重试开销

#### 3.1.4 Hierarchical Timing Wheel

两级时间轮用于超时管理：

```
Level 1: 256 slots × 1ms  = 覆盖 256ms（短超时）
Level 2: 256 slots × 256ms = 覆盖 ~65s（长超时）
Entry Pool: 预分配 1M 个 TimeoutEntry
```

**设计评价**：
- 优点：O(1) 插入和检查，对比 `priority_queue` 的 O(log N) 在 1M QPS 下节省 10%+ 开销
- 缺点：Level 2 → Level 1 降级时的 redistribution 使用了 `reinterpret_cast<uintptr_t>(entry) >> 6) % kLevel1Size` 来分配目标 slot，这个散列方式与实际剩余超时时间无关，可能导致超时不精确（提前或延迟触发）
- 缺点：不支持取消操作（通过 generation 隐式取消），浪费已分配的 entry 直到过期

---

### 3.2 Core 层

#### 3.2.1 RDMA 设备抽象

`RdmaDevice` 封装 `ibv_context`，支持：
- 设备枚举与选择
- NUMA-aware 设备选择（选择与指定 CPU 同 NUMA node 的 NIC）
- 设备属性查询

#### 3.2.2 QueuePair

QP 状态机管理：`RESET → INIT → RTR → RTS`

关键能力：
- Send / SendInline / Write / WriteWithImm / Read / Recv
- **自适应 Signaling**：每 N 个 WR 发送一个 signaled WR，减少 CQE 数量
- **Backpressure**：`CanPostSignaled()` 检查 CQ 深度

**设计评价**：
- 优点：RAII 管理 QP 生命周期，自动状态转换
- 优点：自适应 signaling 减少了 CQ polling 开销
- 缺点：QP 状态转换参数（如 `retry_cnt`, `rnr_retry` 等）硬编码在实现中，不够灵活

#### 3.2.3 Doorbell Batcher

核心优化：将多个 WR 合并为一次 MMIO (Doorbell)。

```
三重触发机制：
1. Batch size 触发：累积 WR 达到 max_batch_size（默认 32）
2. 时间触发：超过 max_delay_us（默认 10μs）
3. 空闲触发：连续 2 次 poll 无事件时 flush

时间测量：使用 RDTSC 指令，亚纳秒精度
```

**设计评价**：
- 优点：将 N 次 MMIO 合并为 1 次，显著降低 doorbell overhead
- 优点：三重触发机制平衡了延迟和吞吐
- 缺点：TSC 频率校准使用 `sleep_for(10ms)` 测量，精度受调度器影响
- 缺点：`tsc_per_us_` 为 static inline 变量，多个 DoorbellBatcher 实例共享同一校准值，如果不同 CPU 核有不同 TSC 频率（虽然现代 CPU 通常 invariant TSC），可能不准确

---

### 3.3 Memory 层

#### 3.3.1 Buffer Pool

```
LocalBufferPool（per-Reactor，无锁）：
- 初始化时一次性预分配所有 buffer
- Ring array (offset_ring_) 支持 FIFO / LIFO 分配策略
- 支持 Hugepage + NUMA binding
- Free 时 prefetch 到 L1 cache
```

**设计评价**：
- 优点：热路径零分配，完全无锁
- 优点：LIFO 策略提高 cache 局部性（最近释放的 buffer 最先被复用）
- 优点：`__builtin_prefetch` 在 free 时预取 buffer 头部，减少下次使用时的 cache miss
- 缺点：固定 buffer 大小，不支持变长分配（需要 SLAB 策略，当前未实现）
- 缺点：buffer 数量在初始化时固定，无法动态扩容

#### 3.3.2 Hugepage Allocator

```cpp
mmap(MAP_HUGETLB | MAP_HUGE_2MB)  // 2MB 大页
mbind(MPOL_BIND, numa_mask)        // NUMA 绑定
```

**设计评价**：
- 优点：减少 TLB miss，对 RDMA 场景尤为关键（RDMA 硬件通过虚拟地址访问内存）
- 优点：NUMA 绑定确保内存与 CPU/NIC 同 node
- 缺点：需要系统预留 hugepage（`/proc/sys/vm/nr_hugepages`），运维成本较高
- 缺点：不支持 1GB hugepage（代码中有 flag 但未完全实现）

---

### 3.4 Transport 层

#### 3.4.1 Flow Control

基于 credit 的流控：

```
发送端：TryConsume() 消费 credit → 允许发送
接收端：OnRecvCompleted() 累积 pending credits

Lazy Update 机制：
- pending_credits > threshold% × recv_depth 时才发送 credit 更新
- 优先通过 response piggyback 传递 credit（零额外消息开销）

自适应阈值：
- 高接收速率 → 降低 threshold（更频繁更新，避免发送端 stall）
- 低接收速率 → 提高 threshold（减少更新消息数）
- 滑动窗口统计接收速率
```

**设计评价**：
- 优点：Lazy update + piggyback 将流控开销降到最低
- 优点：自适应阈值根据负载自动调整，兼顾低延迟和高吞吐
- 缺点：`TryConsume` 使用 relaxed load + store 而非 CAS，在注释中声明"单线程消费"，但使用了 `std::atomic`，存在语义混淆。如果确实是单线程，应该用普通变量以避免误导
- 缺点：credit 耗尽时仅返回 false，无 backoff 或通知机制，调用方需要自己处理重试

#### 3.4.2 TCP Handshake

QP 参数交换走 TCP 通道：

```
Client ──TCP──→ Server
  │ Exchange: QPN, LID, GID, PSN, buffer_addr, rkey │
Server ──TCP──→ Client
```

**设计评价**：
- 优点：简单可靠，不依赖 RDMA CM 的复杂事件模型
- 缺点：管理面仍走 TCP，不支持 RDMA CM 的高级特性（如 path migration）
- 缺点：无 TLS/认证机制，生产环境需额外安全措施

---

### 3.5 RPC 层

#### 3.5.1 Heat-Layered RPC Slot

**核心创新**：将 RPC slot 按访问频率分为三层：

```
RpcSlotHot (16 bytes)          ← 每次 PollCQ 都访问
  slot_gen_packed (20-bit index + 12-bit generation)
  flags (state | wr_type | signaled)
  conn_id, wr_count, error_code, timeout_tick

RpcSlotContext (32 bytes)      ← 仅在 RPC 完成时访问
  callback_fn, callback_ctx
  recv_buffer, recv_length, extended_info_index

ExtendedSlotInfo (64 bytes)    ← 冷数据，调试/统计用
  rpc_id, start_tsc, user_data
  request_size, response_size, wr_details[4]
```

**内存布局效果**：

```
PollCQ 热路径：
  slots_hot_[] → 连续内存，16B/slot，4 slots/cache line
  扫描 1024 slots 仅需 256 个 cache lines (16KB)

对比 flat 设计：
  如果 hot+context+extended 合并 = 112B/slot
  扫描 1024 slots 需 1750+ cache lines (112KB)
  ~7x cache 占用，大量 TLB/cache miss
```

**设计评价**：
- 优点：极致的 cache-friendly 设计，PollCQ 热路径只触碰 hot 数组
- 优点：12-bit generation 防止 ABA 问题（4096 次回收才 wrap）
- 优点：Stack-based O(1) 分配/释放
- 缺点：generation 仅 12 位，在 1M QPS 下约 4.5 分钟 wrap 一次。若 slot 复用间隔短于此，理论上可能出现 ABA（概率极低但非零）
- 缺点：WrId 中 generation 为 8 位（256），而 RpcSlotHot 中为 12 位（4096），存在精度不一致

#### 3.5.2 3-Level Completion Dispatcher

```
L0: 2-way set-associative cache
    32 sets × 2 ways = 64 entries
    cache-line aligned (64B/set)
    LRU 近似淘汰（access_count 递减）

L1: Open-address hash table
    64 entries, 线性探测（max 4 probes）
    Hash: conn_id × 0x9E3779B9 (黄金比例)

L2: Two-level index
    256 buckets × 256 connections
    完整映射，O(1) 但 cache 不友好
```

**设计评价**：
- 优点：在典型场景下（少量活跃连接贡献大部分流量），L0 命中率极高
- 优点：L0 的 SoA 布局（conn_ids[] 连续、handlers[] 连续）对 SIMD 友好
- 优点：多级缓存思想巧妙，类比 CPU 缓存层次
- 缺点：L1 线性探测最多 4 次，高冲突时直接 fallback 到 L2，跳跃较大
- 缺点：L2 使用 `buckets_[256]`，每个 bucket 指向 256 个 `ConnectionEntry`，总共 65536 entries × 16B = 1MB 内存，即使大部分连接不活跃
- 缺点：L0 的 LRU 近似策略（access_count 设为 255 然后递减）粒度较粗

---

### 3.6 Reactor 层

```
Reactor::Poll() 主循环：
  1. Poll CQ → batch dispatch completions
  2. Run custom pollers
  3. Process MPSC task queue
  4. Process timeout wheel
  5. Doorbell flush
  6. Adaptive backoff (if idle)

Adaptive Batch Size：
  LATENCY mode: 8-16 WCE per poll
  THROUGHPUT mode: 32-64 WCE per poll
  ADAPTIVE mode: 根据平均 WCE 数量自动调整

Idle Backoff (x86_64)：
  < 100 spins:   1× _mm_pause()
  < 1000 spins:  4× _mm_pause()
  < max_spin:    16× _mm_pause()
  >= max_spin:   32× _mm_pause()
```

**设计评价**：
- 优点：Run-to-completion 模型，避免上下文切换
- 优点：自适应 batch size 在低负载和高负载间自动调整
- 优点：分级 backoff 在空闲时节省 CPU 功耗，同时保持低唤醒延迟
- 缺点：ARM 平台 backoff 策略较粗糙（仅 `yield()`），可能需要针对性优化
- 缺点：单 Reactor 单 CQ 模型，如果一个连接上有大量 completion，会阻塞其他连接的处理

---

## 4. 关键性能优化汇总

| 优化技术 | 位置 | 效果 |
|----------|------|------|
| Heat-layered data | RpcSlotPool | PollCQ cache 占用降低 7x |
| 3-level cache dispatcher | CompletionDispatcher | 活跃连接 O(1) 分发 |
| Doorbell batching | DoorbellBatcher | MMIO 调用从 N 降为 1 |
| Hierarchical timing wheel | TimingWheel | O(1) 超时管理 |
| ALWAYS_INLINE + HOT_FUNCTION | 所有热路径 | 消除函数调用开销 |
| LIKELY/UNLIKELY | 分支预测 | 减少分支预测失败 |
| __builtin_prefetch | Slot/Buffer 分配 | 减少 cache miss |
| Hugepage + NUMA binding | Memory 层 | 减少 TLB miss |
| Adaptive signaling | QueuePair | 减少 CQE 数量 |
| Credit piggyback | FlowControl | 零额外流控消息 |
| Lock-free MPSC | Reactor 跨线程 | 零锁开销 |
| TSC 时间测量 | DoorbellBatcher | 亚纳秒精度计时 |

---

## 5. 优缺点总结

### 5.1 优点

1. **极致的性能工程**：从数据结构到内存布局，每个细节都为高吞吐低延迟优化。Heat-layered slot、3-level dispatcher、doorbell batching 等设计体现了深厚的系统性能调优经验。

2. **清晰的分层架构**：Common → Core → Memory → Transport → RPC → Reactor 层次分明，职责清晰，代码组织良好。

3. **RAII 资源管理**：所有 RDMA 资源（Device, PD, MR, CQ, QP）都通过 RAII 管理，避免资源泄漏。

4. **可配置性**：几乎所有组件都提供 Config 结构体，支持灵活调参。

5. **NUMA-aware 设计**：从设备选择到内存分配，全链路 NUMA 感知，避免跨 NUMA 访问带来的性能损失。

6. **自适应机制**：batch size、signaling interval、flow control threshold 等参数都能根据负载自适应调整。

7. **预分配策略**：Buffer、Slot、TimingWheelEntry 全部在初始化时预分配，热路径零动态内存分配。

### 5.2 缺点与改进建议

1. **测试缺失**：`tests/` 目录存在但无测试代码。对于如此复杂的框架，单元测试和集成测试是必需的。建议优先补充 core 层的单元测试。

2. **文档不足**：缺乏 API 文档和使用指南。头文件中的注释偏向实现细节而非用户视角。

3. **错误恢复能力弱**：
   - QP 进入 ERROR 状态后没有恢复机制（需要 destroy + recreate）
   - Connection 断开后无自动重连逻辑
   - 建议增加 connection recovery / QP reset 能力

4. **安全性考虑不足**：
   - TCP handshake 无认证/加密
   - 无 rkey 轮换机制（remote key 固定，存在安全风险）
   - 建议增加 mTLS 或 token-based 认证

5. **可观测性不足**：
   - 缺少 metrics 导出（如 Prometheus/OpenTelemetry 集成）
   - Logging 过于简单，缺乏 structured logging
   - 建议增加关键路径的性能计数器

6. **Timing Wheel Level 2 降级精度问题**：Level 2 entry 降级到 Level 1 时，使用指针地址散列而非实际剩余超时时间来分配 slot，导致超时精度在 256ms 范围内随机化。建议记录实际过期时间。

7. **Generation 位宽不一致**：WrId 中 generation 为 8 位，RpcSlotHot 中为 12 位，可能导致验证逻辑不一致。建议统一位宽。

8. **不支持 SRQ (Shared Receive Queue)**：当前每个 QP 独立管理 Recv buffer，在连接数较多时 buffer 消耗大。SRQ 可以跨 QP 共享 Recv buffer，显著减少内存用量。

9. **不支持多 QP per Connection**：高带宽场景下，单 QP 可能成为瓶颈（QP 内部串行化）。建议支持多 QP 聚合。

10. **ARM 平台支持不完善**：虽然有 `__aarch64__` 条件编译分支，但 backoff 策略、内存屏障、prefetch 等未针对 ARM 优化。

---

## 6. 数据流分析

### 6.1 RPC 请求发送流程

```
Application
    │
    ▼
RpcEndpoint::SendRequest()
    │
    ├── RpcSlotPool::Allocate()         ← O(1) stack pop
    │       └── 获取 slot_index + generation
    │
    ├── WrId::Encode(conn_id, opcode, gen, slot_index)
    │
    ├── Connection::Send()              ← header inline
    │       ├── FlowControl::TryConsume()
    │       ├── QueuePair::SendInline()  ← header < 64B 强制 inline
    │       └── DoorbellBatcher::AddSendWr()  ← 不立即提交
    │
    └── TimingWheel::Schedule()         ← 注册超时
```

### 6.2 RPC 完成处理流程

```
Reactor::Poll()
    │
    ├── ibv_poll_cq() → wc_buffer_[]
    │
    ├── CompletionDispatcher::DispatchBatch()
    │       │
    │       ├── L0 cache hit?  ──→ handler(ctx, wc) ──→ done
    │       ├── L1 cache hit?  ──→ handler + MaybePromoteToL0
    │       └── L2 lookup      ──→ handler + InsertHotCache
    │
    ├── RpcEndpoint::OnSendComplete()
    │       ├── RpcSlotHot::MarkCompleted()
    │       ├── IsCompleted()? ──→ callback_fn(ctx, slot_index, status)
    │       └── RpcSlotPool::Free()     ← O(1) stack push
    │
    └── DoorbellBatcher::OnPollEnd()
            └── ShouldFlush()? ──→ ibv_post_send (单次 MMIO)
```

---

## 7. 配置参数参考

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `QueuePairConfig::max_send_wr` | 4096 | 发送队列深度 |
| `QueuePairConfig::max_recv_wr` | 4096 | 接收队列深度 |
| `QueuePairConfig::signal_interval` | 16 | 每 N 个 WR signal 一次 |
| `CompletionQueueConfig::cq_depth` | 65536 | CQ 深度 |
| `ConnectionConfig::recv_buffer_count` | 256 | 预发布 Recv buffer 数 |
| `ConnectionConfig::timeout_ms` | 30000 | 连接超时 (30s) |
| `FlowControlConfig::recv_depth` | 1024 | Flow control 深度 |
| `FlowControlConfig::lazy_update_threshold` | 0.25 | Lazy update 阈值 (25%) |
| `BufferPoolConfig::buffer_size` | 4096 | 单个 buffer 大小 (4KB) |
| `BufferPoolConfig::buffer_count` | 1024 | buffer 总数 |
| `ReactorConfig::latency_batch_size` | 16 | 低延迟模式 batch size |
| `ReactorConfig::throughput_batch_size` | 64 | 高吞吐模式 batch size |
| `ReactorConfig::max_spin_count` | 1000 | 最大空转次数 |
| `DoorbellBatcher::max_batch_size` | 32 | Doorbell batch 上限 |
| `DoorbellBatcher::max_delay_us` | 10 | Doorbell 最大延迟 (10μs) |

---

## 8. 与同类框架对比

| 特性 | 本框架 | eRPC | HERD | FaRM |
|------|--------|------|------|------|
| 连接模式 | RC | RC/UC/UD | UD+UC | RC |
| 完成通知 | Polling | Polling | Polling | Polling |
| Zero-copy | WRITE_WITH_IMM | SEND inline | WRITE | WRITE |
| Flow control | Credit-based | Credit-based | Client-driven | Credit-based |
| 线程模型 | Reactor | Session-based | Worker | Coroutine |
| Doorbell batch | 是 | 是 | 否 | 是 |
| NUMA-aware | 是 | 是 | 部分 | 是 |
| Hugepage | 是 | 是 | 是 | 是 |
| SRQ | 否 | 否 | 否 | 是 |

---

## 9. 未来演进建议

1. **SPDK 集成**：DMA-aware 内存分配，与 NVMe over RDMA 对接
2. **brpc 管理面**：通过 brpc 暴露 metrics、配置热更新、健康检查
3. **多路径支持**：多 NIC / 多 QP 聚合，提升可用性和带宽
4. **Coroutine 支持**：C++20 coroutine 简化异步编程模型
5. **SRQ 支持**：减少大规模连接场景的 buffer 消耗
6. **Connection Recovery**：QP error 后自动重建连接
7. **完善测试**：单元测试 + 集成测试 + 性能基准测试
8. **安全增强**：handshake 认证、rkey 轮换
