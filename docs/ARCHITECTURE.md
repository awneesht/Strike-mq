# BlazeMQ Architecture

## Overview

BlazeMQ is a sub-millisecond, Kafka-compatible message broker written in C++20. It implements the Kafka wire protocol so standard Kafka clients (librdkafka, kafka-python, etc.) can connect directly. The broker is single-threaded with a non-blocking event loop, targeting ultra-low latency on commodity hardware.

## System Architecture

```
                    Kafka Clients (kcat, librdkafka, kafka-python, ...)
                                      |
                                 TCP :9092
                                      |
                    +----------------------------------+
                    |        Network Layer             |
                    |   kqueue (macOS) / epoll (Linux) |
                    |   Non-blocking I/O, per-conn     |
                    |   read/write buffers             |
                    +----------------------------------+
                                      |
                         Kafka Frame Extraction
                       (4-byte length prefix framing)
                                      |
                    +----------------------------------+
                    |        Protocol Layer            |
                    |   RequestRouter dispatches by    |
                    |   ApiKey to handler callbacks    |
                    +----------------------------------+
                         |              |           |
                    +--------+  +-----+  +---------+  +-----------+  +------------+
                    |Produce |  |Fetch|  |Metadata |  |ApiVersions|  |ListOffsets |
                    |Handler |  |     |  |Handler  |  |(built-in) |  |Handler     |
                    +--------+  +-----+  +---------+  +-----------+  +------------+
                         |         |          |
                    +----------------------------------+
                    |        Storage Layer             |
                    |   PartitionLog per topic-partition|
                    |   Memory-mapped log segments     |
                    |   Sparse offset index            |
                    +----------------------------------+
                                      |
                              /tmp/blazemq/data/
                         {topic}-{partition}/0.log
```

## Layer Details

### 1. Network Layer

**Files:** `include/network/tcp_server.h`, `src/network/tcp_server.cpp`

The `TcpServer` class implements a non-blocking event-driven TCP server:

- **macOS:** Uses `kqueue` with `EVFILT_READ`/`EVFILT_WRITE` events
- **Linux:** Uses `epoll` with `EPOLLIN`/`EPOLLOUT` events
- **Connection lifecycle:** accept -> read -> frame -> route -> write -> close
- **Buffering:** Each connection has independent read and write buffers
- **Frame extraction:** Reads the 4-byte big-endian message size prefix, accumulates bytes until a full Kafka frame is available, then routes to the protocol layer
- **TCP tuning:** `SO_REUSEADDR`, `SO_REUSEPORT`, `TCP_NODELAY`, non-blocking sockets

**Key constants:**
| Constant | Value | Purpose |
|----------|-------|---------|
| `kReadChunk` | 64KB | recv() buffer size per read call |
| `kMaxFrameSize` | 100MB | Maximum allowed Kafka frame |
| `kMaxEvents` | 64 | Events per kqueue/epoll iteration |
| `kListenBacklog` | 128 | TCP listen backlog |

### 2. Protocol Layer

**Files:** `include/protocol/kafka_codec.h`, `src/protocol/kafka_codec.cpp`

Implements the Kafka binary wire protocol with four components:

**BinaryReader/BinaryWriter** — Low-level big-endian serialization primitives supporting int8/16/32/64, Kafka strings (int16 length-prefixed), nullable strings, byte arrays (int32 length-prefixed), and signed varints.

**KafkaDecoder** — Stateless decoder for Kafka requests:
- `decode_header()` — Reads ApiKey, api_version, correlation_id, client_id
- `decode_produce()` — Parses topic-partition arrays with record batches (v2 magic)
- `decode_fetch()` — Parses fetch requests with offset/partition info
- `decode_record_batch()` — Full v2 record batch parsing including varints, timestamps, producer metadata; tracks raw byte ranges for zero-copy re-serialization
- `decode_list_offsets()` — Parses ListOffsets v0-v2 with timestamp-based offset queries

**KafkaEncoder** — Stateless encoder for Kafka responses:
- `encode_produce_response()` — Per-partition base offsets and error codes
- `encode_api_versions_response()` — Dual format: v0-v2 (standard) and v3+ (flexible versions with compact arrays and tagged fields)
- `encode_metadata_response()` — Broker list and topic-partition layout (v0 format)
- `encode_fetch_response()` — Version-aware (v0-v4) with zero-copy record batch data from mmap'd segments
- `encode_list_offsets_response()` — Version-aware (v0-v2) with earliest/latest offset resolution

**RequestRouter** — Callback-based request dispatch:
- Routes by `ApiKey` to registered handler functions
- Handles `ApiVersions` inline (special case: v3+ uses flexible headers without header tag_buffer)
- Handlers are `std::function` callbacks set by the broker

**Advertised API versions:**

| API | Key | Versions | Status |
|-----|-----|----------|--------|
| Produce | 0 | 0-5 | Handled |
| Fetch | 1 | 0-4 | Handled |
| ListOffsets | 2 | 0-2 | Handled |
| Metadata | 3 | 0 | Handled |
| OffsetCommit | 8 | 0-3 | Advertised |
| OffsetFetch | 9 | 0-3 | Advertised |
| FindCoordinator | 10 | 0-2 | Advertised |
| JoinGroup | 11 | 0-3 | Advertised |
| Heartbeat | 12 | 0-2 | Advertised |
| LeaveGroup | 13 | 0-1 | Advertised |
| SyncGroup | 14 | 0-2 | Advertised |
| ApiVersions | 18 | 0-3 | Handled |
| CreateTopics | 19 | 0-3 | Advertised |

### 3. Storage Layer

**Files:** `include/storage/partition_log.h` (header-only implementation)

**LogSegment** — A single memory-mapped log file:
- Pre-allocates segments to `max_size` (default 1GB) using `ftruncate` + `mmap`
- Appends are sequential byte copies into the mapped region
- Uses `std::atomic<uint64_t>` for thread-safe write offset tracking
- Linux optimizations: `MADV_HUGEPAGE`, `POSIX_FADV_SEQUENTIAL`
- Async durability via `msync(MS_ASYNC)` in destructor, explicit `sync()` for `MS_SYNC`

**OffsetIndex** — Sparse index for offset-to-position lookup:
- Adds an entry every 4KB boundary
- Binary search via `std::lower_bound` for offset lookup

**PartitionLog** — Manages the lifecycle of log segments for a single topic-partition:
- Auto-creates directory structure: `{log_dir}/{topic}-{partition}/`
- Rolls to a new segment when the active segment reaches 95% capacity
- Serializes `RecordBatch` to Kafka v2 binary format for on-disk storage
- Zero-copy reads: `read()` returns a raw pointer into the mmap'd segment, scanning batch headers to find the target offset and accumulating bytes up to `max_bytes`
- Tracks `next_offset` and `high_watermark` atomically

**Data layout on disk:**
```
/tmp/blazemq/data/
  test-topic-0/
    0.log           # First segment (up to 1GB)
    1048576.log     # Next segment (if first fills up)
```

### 4. Utilities

**Memory Pool** (`include/utils/memory_pool.h`):
- `HugePagePool` — Pre-allocated block allocator
- Allocates a single large region via `mmap` (with `MAP_HUGETLB` on Linux)
- Lock-free freelist using `std::atomic<BlockHeader*>`
- `PoolBuffer` RAII wrapper for automatic deallocation

**Ring Buffers** (`include/utils/ring_buffer.h`):
- `SPSCRingBuffer<T, Capacity>` — Single Producer, Single Consumer
- `MPSCRingBuffer<T, Capacity>` — Multi-Producer, Single Consumer
- Lock-free with `std::memory_order_acquire/release` semantics
- Cache-line aligned (64 bytes) to prevent false sharing
- Batch pop support for drain operations

**Endian Helpers** (`include/utils/endian.h`):
- `bswap16/32/64()` — Uses `__builtin_bswap*` when available, fallback for other compilers

### 5. Broker Orchestration

**File:** `src/main.cpp`

The main function wires all layers together:

1. Initializes `BrokerConfig` with defaults (port 9092, 1GB segments, `/tmp/blazemq/data`)
2. Creates a lazy topic-partition registry (`unordered_map<TopicPartition, PartitionLog>`)
3. Sets up `RequestRouter` handlers:
   - **Produce handler:** Auto-creates `PartitionLog` on first write, appends record batches, returns base offsets
   - **Fetch handler:** Looks up `PartitionLog`, calls `read()` for zero-copy segment data, returns raw mmap'd bytes
   - **ListOffsets handler:** Resolves timestamp queries (-2=earliest, -1=latest) to actual offsets from `PartitionLog`
   - **Metadata handler:** Auto-creates requested topics (like Kafka's `auto.create.topics.enable`), returns broker info and known topics
4. Creates `TcpServer` with the router, binds to `0.0.0.0:9092`
5. Runs the event loop (blocks until SIGINT/SIGTERM)

## Platform Support

| Feature | macOS | Linux |
|---------|-------|-------|
| Event I/O | kqueue | epoll |
| Kernel bypass | N/A | DPDK (optional) |
| Huge pages | N/A | 2MB/1GB pages |
| Async I/O | N/A | io_uring (optional) |
| TSC timing | `cntvct_el0` (ARM64) | `rdtsc` (x86-64) |
| Compiler | Clang/LLVM | Clang/LLVM with LTO |

## Build System

CMake 3.20+ with Ninja generator. Three static library targets plus the main executable:

```
blazemq_core (STATIC)
  src/core/*.cpp + src/protocol/*.cpp + src/storage/*.cpp + src/utils/*.cpp
  Links: pthread, atomic (Linux)

blazemq_network (STATIC)
  src/network/*.cpp
  Links: blazemq_core, DPDK (optional), io_uring (optional)

blazemq (EXECUTABLE)
  src/main.cpp
  Links: blazemq_network, blazemq_core
```

## Configuration

Default configuration from `BrokerConfig` struct (hardcoded, YAML parsing not yet implemented):

```yaml
network:
  bind_address: "0.0.0.0"
  port: 9092

storage:
  log_dir: "/tmp/blazemq/data"
  segment_size: 1073741824    # 1GB

performance:
  message_pool_blocks: 1048576
  message_pool_block_size: 4096
  adaptive_batching: true

cluster:
  broker_id: 0
  replication_factor: 1
```

## Code Metrics

| Component | Headers | Implementation | Tests |
|-----------|---------|----------------|-------|
| Core types | 152 | 2 | - |
| Protocol | 151 | 255 | 41 |
| Storage | 190 | - (header-only) | - |
| Network | 90 | 317 | - |
| Utilities | 307 | - (header-only) | 112 |
| Main | - | 118 | - |
| Benchmarks | - | - | 82 |
| **Total** | **890** | **692** | **235** |
