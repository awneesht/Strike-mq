# BlazeMQ Product Guide

## What is BlazeMQ?

BlazeMQ is a high-performance message broker that speaks the Apache Kafka wire protocol. Any Kafka client library (librdkafka, kafka-python, confluent-kafka-go, etc.) can connect to BlazeMQ and produce/consume messages without code changes.

**Version:** 0.1.0 (Early Development)

## Key Features

- **Kafka protocol compatible** — Drop-in replacement for Kafka for local development and testing
- **Sub-millisecond latency** — Optimized for ultra-low latency with lock-free data structures and memory-mapped storage
- **Zero external dependencies** — Pure C++20, no JVM, no ZooKeeper, no third-party libraries
- **Cross-platform** — Runs on macOS (Apple Silicon + Intel) and Linux (x86-64)
- **Lightweight** — Single 52KB binary, starts in milliseconds, 0% CPU when idle
- **Auto-topic creation** — Topics are created automatically on first produce or metadata request

## Quick Start

### Build

```bash
cd blaze-mq
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

### Run

```bash
./build/blazemq
```

Output:
```
═══════════════════════════════════════════
  BlazeMQ v0.1.0 — Sub-Millisecond Broker
═══════════════════════════════════════════
  Platform: macOS (kqueue)
  Port: 9092
═══════════════════════════════════════════

  Listening on 0.0.0.0:9092
Broker ready. Press Ctrl+C to stop.
```

### Test with kcat

```bash
# Install kcat (Kafka CLI tool)
brew install kcat    # macOS
apt install kcat     # Linux

# List broker metadata
kcat -b 127.0.0.1:9092 -L

# Produce messages
echo "hello world" | kcat -b 127.0.0.1:9092 -P -t my-topic

# Produce multiple messages
echo -e "msg1\nmsg2\nmsg3" | kcat -b 127.0.0.1:9092 -P -t my-topic

# Consume all messages from beginning
kcat -b 127.0.0.1:9092 -C -t my-topic -e

# Consume with consumer group
kcat -b 127.0.0.1:9092 -G my-group my-topic

# Verify topic was created
kcat -b 127.0.0.1:9092 -L
```

### Test with Python

```python
from kafka import KafkaProducer, KafkaConsumer

# Produce
producer = KafkaProducer(bootstrap_servers='127.0.0.1:9092')
producer.send('my-topic', b'hello from python')
producer.flush()

# Consume (simple)
consumer = KafkaConsumer('my-topic', bootstrap_servers='127.0.0.1:9092',
                         auto_offset_reset='earliest')
for msg in consumer:
    print(msg.value.decode())
    break

# Consume with consumer group
consumer = KafkaConsumer('my-topic', group_id='my-group',
                         bootstrap_servers='127.0.0.1:9092',
                         auto_offset_reset='earliest')
for msg in consumer:
    print(msg.value.decode())
    break
```

## Supported Kafka APIs

| API | Support Level | Notes |
|-----|--------------|-------|
| ApiVersions | Full | v0-v3 including flexible versions |
| Metadata | Full | v0, auto-creates topics |
| Produce | Full | v0-v5, persists to disk |
| Fetch | Full | v0-v4, zero-copy reads from mmap'd segments |
| ListOffsets | Full | v0-v2, resolves earliest/latest offsets |
| OffsetCommit | Full | v0-v3, in-memory offset storage |
| OffsetFetch | Full | v0-v3, retrieve committed offsets |
| FindCoordinator | Full | v0-v2, returns self as coordinator |
| JoinGroup | Full | v0-v3, rebalance protocol with member assignment |
| SyncGroup | Full | v0-v2, leader distributes partition assignments |
| Heartbeat | Full | v0-v2, session management and rebalance signaling |
| LeaveGroup | Full | v0-v1, clean consumer shutdown |
| CreateTopics | Advertised | Negotiation only |

"Advertised" means the API is listed in the ApiVersions response (required for client compatibility) but returns a minimal response.

## Storage

Messages are stored in memory-mapped log files under `/tmp/blazemq/data/`:

```
/tmp/blazemq/data/
  my-topic-0/
    0.log         # 1GB pre-allocated segment
  another-topic-0/
    0.log
```

Each topic-partition gets its own directory with rolling log segments. Segments are pre-allocated to 1GB for sequential I/O performance.

## Performance Characteristics

| Metric | Target | Notes |
|--------|--------|-------|
| Produce latency (p99.9) | < 1ms | Single-threaded event loop |
| CPU idle | 0% | Event-driven, no busy polling |
| Memory footprint | ~1MB + segments | Minimal base, segments are mmap'd |
| Startup time | < 10ms | No JVM, no warmup |
| Binary size | 52KB | Statically linked core |

Run the built-in benchmarks:
```bash
./build/blazemq_bench
```

## Limitations (v0.1.0)

- **Consumer group offsets are in-memory** — Committed offsets are lost on broker restart
- **No replication** — Single broker, no fault tolerance
- **No authentication** — No SASL/SSL support
- **No YAML config loading** — Uses hardcoded defaults from BrokerConfig struct
- **Single-threaded** — One event loop thread handles all connections
- **IPv4 only** — No IPv6 listener support
- **No log compaction** — Segments accumulate indefinitely
- **No retention enforcement** — `retention_ms` configured but not enforced

## Running Tests

```bash
cd build

# Run all tests
ctest

# Run individual tests
./blazemq_test_ring    # Lock-free ring buffer tests
./blazemq_test_pool    # Memory pool allocation tests
./blazemq_test_codec   # Kafka protocol codec tests

# Run benchmarks
./blazemq_bench        # Latency microbenchmarks
```

## Configuration Reference

Configuration is currently set via the `BrokerConfig` struct defaults in `include/core/types.h`. A YAML configuration loader is planned.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `bind_address` | `0.0.0.0` | Network interface to bind |
| `port` | `9092` | Kafka protocol port |
| `num_io_threads` | `0` (auto) | I/O threads (future use) |
| `log_dir` | `/tmp/blazemq/data` | Partition log directory |
| `segment_size` | 1GB | Max size per log segment |
| `retention_ms` | 7 days | Message retention (not enforced yet) |
| `message_pool_blocks` | 1M | Pre-allocated buffer pool blocks |
| `message_pool_block_size` | 4KB | Size of each pool block |
| `broker_id` | `0` | Broker ID in cluster metadata |
| `replication_factor` | `1` | Replication factor (single broker) |

## Stopping the Broker

Send `SIGINT` (Ctrl+C) or `SIGTERM` for a graceful shutdown. Open connections are closed, and pending writes are flushed.
