# StrikeMQ Architecture

## Overview

StrikeMQ is a sub-millisecond, Kafka-compatible message broker written in C++20, designed for local development and testing. It implements the Kafka wire protocol so standard Kafka clients (librdkafka, kafka-python, etc.) can connect directly. A built-in REST API on port 8080 allows inspection and control with just `curl`. The broker uses a multi-threaded acceptor + N worker architecture with non-blocking event loops (kqueue/epoll/io_uring), sharded storage with 64 cache-line-aligned lock shards, and circular I/O buffers — targeting ultra-low latency on commodity hardware.

## System Architecture

```
  Kafka Clients (kcat, librdkafka, ...)         curl / HTTP clients
                    |                                     |
               TCP :9092                            TCP :8080
                    |                                     |
  +----------------------------------+    +----------------------------------+
  |    Acceptor Thread (TcpServer)   |    |    HttpServer (single thread)    |
  |  kqueue/epoll — accept() only    |    |  kqueue/epoll event loop         |
  |  Round-robin to workers          |    |  HTTP/1.1 parser + JSON writer   |
  +----------------------------------+    +----------------------------------+
      |        |        |        |                        |
 SPSC ring buffers (lock-free fd handoff)       HttpRouter (path matching)
      |        |        |        |           /v1/broker, /v1/topics, ...
  Worker 0  Worker 1   ...   Worker N-1               /v1/groups
  (own kqueue/epoll, own connections)                     |
      |        |        |        |                        |
       Kafka Frame Extraction                             |
     (4-byte length prefix framing)                       |
                    |                                     |
  +----------------------------------+                    |
  |        Protocol Layer            |                    |
  |   RequestRouter dispatches by    |                    |
  |   ApiKey to handler callbacks    |                    |
  +----------------------------------+                    |
       |              |           |                       |
  +--------+  +-----+  +---------+  +-----------+        |
  |Produce |  |Fetch|  |Metadata |  |ApiVersions|        |
  |Handler |  |     |  |Handler  |  |(built-in) |        |
  +--------+  +-----+  +---------+  +-----------+        |
       |         |          |                             |
  +----------------------------------+                    |
  |     Consumer Group Handlers       |                   |
  |  FindCoordinator, JoinGroup,      |                   |
  |  SyncGroup, Heartbeat, LeaveGroup,|                   |
  |  OffsetCommit, OffsetFetch        |                   |
  +----------------------------------+                    |
       |         |          |                             |
       +-------- | ---------+-----------------------------+
                 |
  +------------------------------------------+
  |           Shared Storage Layer            |
  |   ShardedLogMap<64> (64 shards)           |
  |   PartitionLog per topic-partition        |
  |   (per-partition append mutex)            |
  |   ConsumerGroupManager (in-memory)        |
  |   Memory-mapped log segments              |
  |   Sparse offset index                     |
  +------------------------------------------+
                    |
            /tmp/strikemq/data/
       {topic}-{partition}/0.log
```

## Layer Details

### 1. Network Layer

**Files:** `include/network/tcp_server.h`, `include/network/io_uring_defs.h`, `src/network/tcp_server.cpp`

The network layer uses an **acceptor + N worker threads** architecture with two main classes:

**`TcpServer` (Acceptor Thread)** — Owns the listen socket and runs an accept-only event loop:
- Calls `accept()` on incoming connections, sets `TCP_NODELAY` and non-blocking mode
- Optionally sets `SO_BUSY_POLL` on accepted sockets (Linux) for reduced wakeup latency
- Round-robin distributes accepted fds to worker threads via lock-free SPSC ring buffers
- Falls back to other workers if the target worker's ring buffer is full
- N defaults to `std::thread::hardware_concurrency()` (configurable via `BrokerConfig::num_io_threads`)

**`WorkerThread` (I/O Thread)** — Each worker runs its own event loop with independent state:
- Own `kqueue` (macOS), `epoll` (Linux), or `io_uring` (Linux with `STRIKE_IO_URING`) instance
- Own `connections_` map — connection state is thread-local, no sharing between workers
- Connection I/O uses `FixedCircularBuffer` (128KB, power-of-2 capacity, O(1) advance) instead of `std::vector<uint8_t>`
- Pipe-based wakeup mechanism: acceptor writes a byte to the pipe after pushing an fd to the ring buffer, worker drains the pipe and registers new fds with its event loop
- Handles all read/write/frame-extraction/routing for its assigned connections
- Shares the `RequestRouter` (read-only after setup) and storage layer (sharded-lock-protected) with other workers

**io_uring event loop** (Linux, `STRIKE_IO_URING`):
- Attempts SQPOLL setup first (kernel-side submission thread, 1ms idle timeout), falls back to normal mode if insufficient privileges
- Submission-based I/O: `io_uring_prep_recv` / `io_uring_prep_send` instead of `recv()`/`send()` syscalls
- Uses `io_uring_wait_cqe_timeout` with 1ms timeout for completion harvesting
- User-data encoding packs `UringOp` type + fd into 64-bit value for zero-lookup dispatch
- Pipe wakeup for new-connection notification (poll_add on wakeup pipe)

**Connection lifecycle:** accept (acceptor) → SPSC handoff → drain + register (worker) → read → frame → route → write → close

**Event loop timeouts:** All event loops (kqueue `kevent`, epoll `epoll_wait`, io_uring `wait_cqe_timeout`, acceptor) use a **1ms timeout** for low-latency responsiveness.

**Key constants:**
| Constant | Value | Purpose |
|----------|-------|---------|
| `kReadChunk` | 64KB | recv() buffer size per read call |
| `kMaxFrameSize` | 100MB | Maximum allowed Kafka frame |
| `kMaxEvents` | 64 | Events per kqueue/epoll iteration |
| `kListenBacklog` | 128 | TCP listen backlog |
| SPSC capacity | 1024 | New-fd ring buffer per worker |
| Connection buffer | 128KB | FixedCircularBuffer per read/write direction |

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
- `decode_find_coordinator()` — Parses group_id and coordinator_type (v1+)
- `decode_join_group()` — Parses group membership with session/rebalance timeouts and protocol metadata (BYTES)
- `decode_sync_group()` — Parses leader assignments (member_id + BYTES assignment pairs)
- `decode_heartbeat()` — Parses group_id, generation_id, member_id
- `decode_leave_group()` — Parses group_id, member_id
- `decode_offset_commit()` — Parses version-aware offset commits (v1 adds timestamp, v2+ adds retention)
- `decode_offset_fetch()` — Parses group_id with topic-partition arrays

**KafkaEncoder** — Stateless encoder for Kafka responses:
- `encode_produce_response()` — Per-partition base offsets and error codes
- `encode_api_versions_response()` — Dual format: v0-v2 (standard) and v3+ (flexible versions with compact arrays and tagged fields)
- `encode_metadata_response()` — Broker list and topic-partition layout (v0 format)
- `encode_fetch_response()` — Version-aware (v0-v4) with zero-copy record batch data from mmap'd segments
- `encode_list_offsets_response()` — Version-aware (v0-v2) with earliest/latest offset resolution
- `encode_find_coordinator_response()` — Version-aware (v0-v2), returns coordinator broker info
- `encode_join_group_response()` — Version-aware (v0-v3), generation_id, leader, members with BYTES metadata
- `encode_sync_group_response()` — Version-aware (v0-v2), member assignment as BYTES
- `encode_heartbeat_response()` — Version-aware (v0-v2), error code for rebalance signaling
- `encode_leave_group_response()` — Version-aware (v0-v1), error code
- `encode_offset_commit_response()` — Version-aware (v0-v3), per-partition error codes
- `encode_offset_fetch_response()` — Version-aware (v0-v3), per-partition offsets with group-level error (v2+)

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
| OffsetCommit | 8 | 0-3 | Handled |
| OffsetFetch | 9 | 0-3 | Handled |
| FindCoordinator | 10 | 0-2 | Handled |
| JoinGroup | 11 | 0-3 | Handled |
| Heartbeat | 12 | 0-2 | Handled |
| LeaveGroup | 13 | 0-1 | Handled |
| SyncGroup | 14 | 0-2 | Handled |
| ApiVersions | 18 | 0-3 | Handled |
| CreateTopics | 19 | 0-3 | Advertised |

### 3. Storage Layer

**Files:** `include/storage/partition_log.h`, `include/storage/sharded_log_map.h`, `include/storage/consumer_group.h` (header-only implementations)

**ShardedLogMap<64>** — Sharded topic-partition registry replacing the global `logs_mu` + `unordered_map`:
- 64 cache-line-aligned (`alignas(64)`) shards, each with its own `std::mutex` and `unordered_map<TopicPartition, unique_ptr<PartitionLog>>`
- Shard selection via `TopicPartitionHash(tp) & 63` (power-of-2 mask)
- `find()` — O(1) lookup, locks only one shard
- `get_or_create()` — Lazy partition creation with callback for new-topic registration
- `for_each()` — Iterates all logs, locking each shard sequentially
- `erase_if()` — Predicate-based removal across all shards (used by topic deletion)
- Concurrent Produce/Fetch/ListOffsets to different shards never contend on the same mutex

**LogSegment** — A single memory-mapped log file:
- Pre-allocates segments to `max_size` (default 1GB) using `ftruncate` + `mmap`
- Appends are sequential byte copies into the mapped region
- Uses `std::atomic<uint64_t>` for thread-safe write offset tracking
- Linux optimizations: `MAP_POPULATE` (avoids TLB misses on initial writes), `MADV_HUGEPAGE`, `POSIX_FADV_SEQUENTIAL`, and pre-faulting the first 2MB of each new segment
- Async durability via `msync(MS_ASYNC)` in destructor, explicit `sync()` for `MS_SYNC`

**OffsetIndex** — Sparse index for offset-to-position lookup:
- Adds an entry every 4KB boundary
- Binary search via `std::lower_bound` for offset lookup

**PartitionLog** — Manages the lifecycle of log segments for a single topic-partition:
- Auto-creates directory structure: `{log_dir}/{topic}-{partition}/`
- Rolls to a new segment when the active segment reaches 95% capacity
- Serializes `RecordBatch` to Kafka v2 binary format for on-disk storage
- Thread-safe `append()` via per-partition `std::mutex` — concurrent writes to different partitions never contend
- Lock-free `read()` — returns a raw pointer into the mmap'd segment, scanning batch headers to find the target offset and accumulating bytes up to `max_bytes`. Uses only `memory_order_acquire` loads on atomics, no mutex needed.
- Tracks `next_offset` and `high_watermark` atomically

**ConsumerGroupManager** — In-memory consumer group coordination:
- Manages group state machine: Empty → PreparingRebalance → CompletingRebalance → Stable
- `join_group()` — Generates member IDs, triggers rebalance, elects leader, chooses protocol by vote
- `sync_group()` — Leader distributes partition assignments, transitions group to Stable
- `heartbeat()` — Validates generation/member, checks session timeouts, signals rebalance via error code 27
- `leave_group()` — Removes member, triggers rebalance or transitions to Empty
- `offset_commit()`/`offset_fetch()` — In-memory offset storage per (group, topic, partition)
- Lazy session timeout checks on heartbeat calls (no background timer threads)
- Thread-safe via `std::mutex` (sufficient for single-broker dev/test use)

**Data layout on disk:**
```
/tmp/strikemq/data/
  test-topic-0/
    0.log           # First segment (up to 1GB)
    1048576.log     # Next segment (if first fills up)
```

### 4. HTTP / REST Layer

**Files:** `include/http/http_server.h`, `src/http/http_server.cpp`

The REST API runs on its own thread with a single-threaded kqueue/epoll event loop (no multi-worker architecture needed for an admin API). It shares storage state with the Kafka protocol path through a `BrokerContext` struct.

**BrokerContext** — Bundles references to all shared broker state:
- `sharded_logs` — `ShardedLogMap<64>` topic-partition registry (per-shard mutex)
- `known_topics` + `topics_mu` — Topic name list (mutex-protected)
- `group_mgr` — Consumer group state (internal mutex)
- `config` — Broker configuration (read-only after startup)
- `get_or_create_log` — Function pointer for lazy partition creation (used by POST produce)

**HttpServer** — Single-threaded HTTP/1.1 server:
- Same kqueue/epoll pattern as `TcpServer` but handles both accept and I/O in one thread
- Parses HTTP request line, headers, and Content-Length-based body
- Connection: close semantics (no keep-alive) — simple and sufficient for admin use
- Routes requests by method + path with `{name}` and `{id}` path parameters
- Query parameter parsing with URL decoding
- Max body size: 1MB (prevents accidental abuse)
- CORS header (`Access-Control-Allow-Origin: *`) for browser-based tools

**JsonWriter** — Minimal JSON serializer (no library dependency):
- Streaming builder with `object_begin/end`, `array_begin/end`, `key`, `value` methods
- Comma tracking via a stack of flags (handles nested objects/arrays correctly)
- JSON string escaping for control characters, backslashes, and quotes

**Route handlers:**

| Handler | Method + Path | Shared State Used |
|---------|---------------|-------------------|
| `handle_get_broker` | GET `/v1/broker` | `config` (read-only) |
| `handle_get_topics` | GET `/v1/topics` | `sharded_logs.for_each()` (per-shard lock) |
| `handle_get_topic` | GET `/v1/topics/{name}` | `sharded_logs.find()` (single-shard lock) |
| `handle_delete_topic` | DELETE `/v1/topics/{name}` | `sharded_logs.erase_if()` + `known_topics` (lock both) |
| `handle_get_messages` | GET `/v1/topics/{name}/messages` | `sharded_logs.find()` (single-shard lock), reads mmap'd segments |
| `handle_post_messages` | POST `/v1/topics/{name}/messages` | `get_or_create_log` (acquires shard lock internally) |
| `handle_get_groups` | GET `/v1/groups` | `group_mgr.list_groups()` (internal mutex) |
| `handle_get_group` | GET `/v1/groups/{id}` | `group_mgr.get_group()` (internal mutex) |

**Message peek** (`handle_get_messages`) reuses the same batch scanning logic as `PartitionLog::read()` — reads baseOffset + batchLength headers from mmap'd segment data, then parses individual records including varint-encoded fields (key length, value length, offset delta, timestamp delta).

**Message produce** (`handle_post_messages`) parses a simple JSON body (`{"messages":[{"key":"...","value":"..."}]}`), builds Kafka v2 record batch format with proper zigzag-encoded varints, and appends via `PartitionLog::append()`. Messages produced via REST are fully wire-compatible with Kafka consumers.

**Key constants:**
| Constant | Value | Purpose |
|----------|-------|---------|
| `kMaxEvents` | 32 | Events per kqueue/epoll iteration |
| `kListenBacklog` | 16 | TCP listen backlog |
| `kMaxBodySize` | 1MB | Maximum POST body size |

### 5. Utilities

**Memory Pool** (`include/utils/memory_pool.h`):
- `HugePagePool` — Pre-allocated block allocator
- Allocates a single large region via `mmap` (with `MAP_HUGETLB` on Linux)
- Lock-free freelist using `std::atomic<BlockHeader*>`
- `PoolBuffer` RAII wrapper for automatic deallocation

**Ring Buffers** (`include/utils/ring_buffer.h`):
- `SPSCRingBuffer<T, Capacity>` — Single Producer, Single Consumer. Used by the network layer to hand off accepted file descriptors from the acceptor thread to each worker thread (one ring buffer per worker).
- `MPSCRingBuffer<T, Capacity>` — Multi-Producer, Single Consumer
- Lock-free with `std::memory_order_acquire/release` semantics
- Cache-line aligned (64 bytes) to prevent false sharing
- Batch pop support for drain operations (`try_pop_batch` used by workers to drain new fds)

**Circular Buffer** (`include/utils/circular_buffer.h`):
- `FixedCircularBuffer` — Fixed-capacity ring buffer replacing `std::vector<uint8_t>` for connection read/write buffers
- Power-of-2 capacity with monotonic head/tail counters and bitmask wrapping — all operations O(1)
- Default 128KB capacity, page-aligned allocation (4096 bytes) for io_uring registered buffer compatibility
- `write_head()` / `read_head()` return contiguous pointer + length for direct `recv()` / `send()` targets
- Supports external memory (no-alloc mode) for io_uring pre-registered buffer pools
- Eliminates the `erase(begin, begin+n)` reallocation overhead of `std::vector` after each `send()`

**Endian Helpers** (`include/utils/endian.h`):
- `bswap16/32/64()` — Uses `__builtin_bswap*` when available, fallback for other compilers

### 6. Broker Orchestration

**File:** `src/main.cpp`

The main function wires all layers together:

1. Initializes `BrokerConfig` with defaults (port 9092, HTTP port 8080, 1GB segments, `/tmp/strikemq/data`)
2. Creates a `ShardedLogMap<64>` — 64-shard topic-partition registry with per-shard locking
3. Creates a `ConsumerGroupManager` for in-memory group state
4. Sets up `RequestRouter` handlers:
   - **Produce handler:** Auto-creates `PartitionLog` via `sharded_logs.get_or_create()`, appends record batches, returns base offsets
   - **Fetch handler:** Looks up via `sharded_logs.find()`, calls `read()` for zero-copy segment data, returns raw mmap'd bytes
   - **ListOffsets handler:** Resolves timestamp queries (-2=earliest, -1=latest) to actual offsets from `PartitionLog`
   - **Metadata handler:** Auto-creates requested topics (like Kafka's `auto.create.topics.enable`), returns broker info and known topics
   - **FindCoordinator handler:** Returns self (single broker is always the coordinator)
   - **JoinGroup/SyncGroup/Heartbeat/LeaveGroup handlers:** Delegate to `ConsumerGroupManager`
   - **OffsetCommit/OffsetFetch handlers:** Delegate to `ConsumerGroupManager`
5. Creates `BrokerContext` bundling `sharded_logs` and other shared state, starts `HttpServer` on port 8080 (own thread)
6. Creates `TcpServer` with the router and `num_io_threads`, binds to `0.0.0.0:9092`
7. Runs the acceptor event loop — starts N worker threads, then blocks until SIGINT/SIGTERM
8. On shutdown (SIGINT/SIGTERM): stops the HTTP server, then the Kafka server and workers

## Platform Support

| Feature | macOS | Linux |
|---------|-------|-------|
| Event I/O | kqueue | epoll |
| Kernel bypass | N/A | io_uring (SQPOLL, registered buffers) |
| Huge pages | N/A | 2MB/1GB pages |
| Async I/O | N/A | io_uring (submission-based recv/send) |
| Busy poll | N/A | `SO_BUSY_POLL` (optional) |
| Segment pre-fault | N/A | `MAP_POPULATE` + 2MB pre-fault |
| TSC timing | `cntvct_el0` (ARM64) | `rdtsc` (x86-64) |
| Compiler | Clang/LLVM | Clang/LLVM with LTO |

## Build System

CMake 3.20+ with Ninja generator. Four static library targets plus the main executable:

```
strikemq_core (STATIC)
  src/core/*.cpp + src/protocol/*.cpp + src/storage/*.cpp + src/utils/*.cpp
  Links: Threads::Threads, atomic (Linux)

strikemq_network (STATIC)
  src/network/*.cpp
  Links: strikemq_core, DPDK (optional), io_uring (optional)

strikemq_http (STATIC)
  src/http/*.cpp
  Links: strikemq_core

strikemq (EXECUTABLE)
  src/main.cpp
  Links: strikemq_network, strikemq_http, strikemq_core
```

## Configuration

Default configuration from `BrokerConfig` struct (hardcoded, YAML parsing not yet implemented):

```yaml
network:
  bind_address: "0.0.0.0"
  port: 9092
  http_port: 8080
  num_io_threads: 0          # 0 = auto (hardware_concurrency)

storage:
  log_dir: "/tmp/strikemq/data"
  segment_size: 1073741824    # 1GB

performance:
  message_pool_blocks: 1048576
  message_pool_block_size: 4096
  adaptive_batching: true
  busy_poll: false              # SO_BUSY_POLL on accepted sockets (Linux)
  io_uring_sq_entries: 256      # io_uring submission queue size
  io_uring_buf_count: 512       # io_uring registered buffer count

cluster:
  broker_id: 0
  replication_factor: 1
```

## Code Metrics

| Component | Headers | Implementation | Tests |
|-----------|---------|----------------|-------|
| Core types | 305 | - | - |
| Protocol | 207 | 716 | 40 |
| Storage | 823 | - (header-only) | 233 |
| Network | 166 | 684 | - |
| HTTP/REST | 158 | 1094 | - |
| Utilities | 480 | - (header-only) | 324 |
| Main | - | 215 | - |
| Benchmarks | - | - | 81 |
| **Total** | **2139** | **2709** | **678** |
