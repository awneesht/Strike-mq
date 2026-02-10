# StrikeMQ Product Guide

## What is StrikeMQ?

StrikeMQ is a lightweight Kafka-compatible message broker designed for **local development and testing**. Any Kafka client library (librdkafka, kafka-python, confluent-kafka-go, etc.) can connect to StrikeMQ and produce/consume messages without code changes. A built-in REST API on port 8080 lets you inspect and control the broker with just `curl`.

Think of it like [LocalStack](https://localstack.cloud/) for AWS or SQLite for PostgreSQL — develop locally against StrikeMQ, deploy to real Kafka in production.

**Version:** 0.1.4 (Early Development)

## Key Features

- **Kafka protocol compatible** — Drop-in replacement for Kafka for local development and testing
- **Built-in REST API** — Inspect topics, peek at messages, produce, and manage consumer groups with just `curl` on port 8080
- **Sub-millisecond latency** — Optimized for ultra-low latency with lock-free data structures and memory-mapped storage
- **Zero external dependencies** — Pure C++20, no JVM, no ZooKeeper, no third-party libraries
- **Cross-platform** — Runs on macOS (Apple Silicon + Intel) and Linux (x86-64)
- **Lightweight** — Single 52KB binary, starts in milliseconds, 0% CPU when idle
- **Auto-topic creation** — Topics are created automatically on first produce or metadata request

## Quick Start

### Docker (easiest)

```bash
docker run -p 9092:9092 -p 8080:8080 awneesh/strikemq
```

### Homebrew (macOS / Linux)

```bash
brew tap awneesht/strike-mq
brew install strikemq
strikemq
```

### Build from source

```bash
cd strike-mq
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

### Run

```bash
./build/strikemq
```

Output:
```
═══════════════════════════════════════════
  StrikeMQ v0.1.4 — Sub-Millisecond Broker
═══════════════════════════════════════════
  Platform: macOS (kqueue)
  Kafka Port: 9092
  HTTP API: 8080
  IO Threads: 10 (auto)
═══════════════════════════════════════════

  HTTP API listening on 0.0.0.0:8080
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

## REST API

StrikeMQ includes a built-in REST API on port **8080**, enabling broker inspection and control without any Kafka client tooling. All responses are JSON. Errors return `{"error": "message"}` with appropriate HTTP status codes.

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/v1/broker` | Broker info: version, uptime, ports, IO threads, platform, config |
| GET | `/v1/topics` | List all topics with partition count and start/end offsets |
| GET | `/v1/topics/{name}` | Topic detail with per-partition start and end offsets |
| DELETE | `/v1/topics/{name}` | Delete a topic and all its data from disk |
| GET | `/v1/topics/{name}/messages?offset=0&limit=10` | Peek at messages as JSON (optional `partition` param) |
| POST | `/v1/topics/{name}/messages` | Produce messages via JSON body (optional `partition` param) |
| GET | `/v1/groups` | List consumer groups with state and member count |
| GET | `/v1/groups/{id}` | Group detail: members, generation, protocol, committed offsets |

### Producing via REST

Send a POST request with a JSON body containing a `messages` array. Each message can have an optional `key` and a required `value`:

```bash
curl -X POST localhost:8080/v1/topics/my-topic/messages \
  -d '{"messages":[{"value":"hello"},{"key":"user-1","value":"world"}]}'
```

Response:
```json
{"topic":"my-topic","partition":0,"base_offset":0,"record_count":2}
```

Messages produced via REST use standard Kafka v2 record batch format and are fully consumable by any Kafka client.

### Peeking at Messages

Read messages without a consumer group — useful for debugging and inspection:

```bash
curl "localhost:8080/v1/topics/my-topic/messages?offset=0&limit=3"
```

Response:
```json
[
  {"offset":0,"timestamp":1234567890,"key":null,"value":"hello"},
  {"offset":1,"timestamp":1234567890,"key":"user-1","value":"world"}
]
```

Query parameters:
- `offset` — Starting offset (default: 0)
- `limit` — Maximum number of messages to return (default: 10, max: 1000)
- `partition` — Partition number (default: 0)

### Inspecting Consumer Groups

View consumer groups created by Kafka clients:

```bash
# List all groups
curl localhost:8080/v1/groups

# Group detail with members and committed offsets
curl localhost:8080/v1/groups/my-group
```

### Deleting Topics

Remove a topic and all its data from disk:

```bash
curl -X DELETE localhost:8080/v1/topics/my-topic
```

Response:
```json
{"deleted":"my-topic","partitions_removed":1}
```

### Interoperability

The REST API and Kafka protocol share the same storage layer. Messages produced via REST are visible to Kafka consumers, and messages produced via Kafka clients are visible through the REST peek endpoint. Topics created through either path appear in both `kcat -L` and `GET /v1/topics`.

## Storage

Messages are stored in memory-mapped log files under `/tmp/strikemq/data/`:

```
/tmp/strikemq/data/
  my-topic-0/
    0.log         # 1GB pre-allocated segment
  another-topic-0/
    0.log
```

Each topic-partition gets its own directory with rolling log segments. Segments are pre-allocated to 1GB for sequential I/O performance.

## Performance Characteristics

| Metric | Target | Notes |
|--------|--------|-------|
| Produce latency (p99.9) | < 1ms | Multi-threaded, per-partition mutex only |
| CPU idle | 0% | Event-driven, no busy polling |
| Memory footprint | ~1MB + segments | Minimal base, segments are mmap'd |
| Startup time | < 10ms | No JVM, no warmup |
| Binary size | 52KB | Statically linked core |

Run the built-in benchmarks:
```bash
./build/strikemq_bench
```

## Limitations (v0.1.4)

StrikeMQ is designed for local development and testing, not production. It trades durability and fault-tolerance for simplicity and speed.

- **Consumer group offsets are in-memory** — Committed offsets are lost on broker restart
- **No replication** — Single broker, no fault tolerance
- **No authentication** — No SASL/SSL support
- **No YAML config loading** — Uses hardcoded defaults from BrokerConfig struct
- **IPv4 only** — No IPv6 listener support
- **No log compaction** — Segments accumulate indefinitely
- **No retention enforcement** — `retention_ms` configured but not enforced

## Running Tests

```bash
cd build

# Run all tests
ctest

# Run individual tests
./strikemq_test_ring    # Lock-free ring buffer tests
./strikemq_test_pool    # Memory pool allocation tests
./strikemq_test_codec   # Kafka protocol codec tests

# Run benchmarks
./strikemq_bench        # Latency microbenchmarks
```

## Configuration Reference

Configuration is currently set via the `BrokerConfig` struct defaults in `include/core/types.h`. A YAML configuration loader is planned.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `bind_address` | `0.0.0.0` | Network interface to bind |
| `port` | `9092` | Kafka protocol port |
| `http_port` | `8080` | REST API port |
| `num_io_threads` | `0` (auto) | Worker threads for I/O (0 = `hardware_concurrency`) |
| `log_dir` | `/tmp/strikemq/data` | Partition log directory |
| `segment_size` | 1GB | Max size per log segment |
| `retention_ms` | 7 days | Message retention (not enforced yet) |
| `message_pool_blocks` | 1M | Pre-allocated buffer pool blocks |
| `message_pool_block_size` | 4KB | Size of each pool block |
| `broker_id` | `0` | Broker ID in cluster metadata |
| `replication_factor` | `1` | Replication factor (single broker) |

## Stopping the Broker

Send `SIGINT` (Ctrl+C) or `SIGTERM` for a graceful shutdown. The HTTP server stops first, then the Kafka server and its worker threads. Open connections are closed, and pending writes are flushed.
