<p align="center">
  <img src="docs/logo.svg" alt="StrikeMQ" width="200">
</p>

<h1 align="center">StrikeMQ</h1>

<p align="center">
  Sub-millisecond, Kafka-compatible message broker written in C++20.<br>
  Zero dependencies. Single binary. Drop-in Kafka replacement for development and testing.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-0.1.5-orange" alt="Version">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue" alt="C++20">
  <img src="https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey" alt="Platform">
  <img src="https://img.shields.io/badge/license-MIT-green" alt="License">
</p>

<p align="center">
  <img src="docs/demo.gif" alt="StrikeMQ Demo" width="700">
</p>

---

## Why StrikeMQ?

Kafka is powerful but heavy — JVM, ZooKeeper/KRaft, gigabytes of RAM, slow startup. For local development and testing, you just need something that speaks the Kafka protocol and gets out of the way.

**Develop locally against StrikeMQ, deploy to real Kafka in production.** Think of it like [LocalStack](https://localstack.cloud/) for AWS or SQLite for PostgreSQL — a lightweight stand-in that keeps your dev loop fast while your production stack stays unchanged.

StrikeMQ is a **52KB native binary** that starts in milliseconds, uses 0% CPU when idle, and works with any Kafka client library out of the box.

## Features

- **Kafka wire protocol** — Works with librdkafka, kafka-python, confluent-kafka-go, kcat, and any Kafka client
- **Built-in REST API** — Inspect topics, peek at messages, produce, and manage consumer groups with just `curl`
- **Sub-millisecond produce latency** — Sharded locks, circular I/O buffers, memory-mapped storage, zero-copy where possible
- **Zero dependencies** — Pure C++20, no JVM, no ZooKeeper, no third-party libraries
- **Multi-threaded I/O** — Acceptor + N worker threads, each with own kqueue (macOS) / epoll (Linux) / io_uring (Linux), 0% CPU when idle
- **io_uring support** — Optional Linux kernel bypass with SQPOLL, registered buffers, and submission-based I/O
- **Consumer groups** — JoinGroup, SyncGroup, Heartbeat, OffsetCommit/Fetch — full rebalance protocol
- **Auto-topic creation** — Topics created on first produce or metadata request
- **Cross-platform** — macOS (Apple Silicon + Intel) and Linux (x86-64)

## Quick Start

### Docker (easiest)

```bash
docker run -p 9092:9092 -p 8080:8080 awneesh/strikemq
```

That's it — any Kafka client can connect to `127.0.0.1:9092`, and the REST API is available at `127.0.0.1:8080`.

### Homebrew (macOS / Linux)

```bash
brew tap awneesht/strike-mq
brew install strikemq
strikemq
```

### Build from source

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

### Run

```bash
./build/strikemq
```

```
═══════════════════════════════════════════
  StrikeMQ v0.1.5 — Sub-Millisecond Broker
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

### Produce and Consume with kcat

```bash
# List broker metadata
kcat -b 127.0.0.1:9092 -L

# Produce messages
echo -e "hello\nworld\nstrike" | kcat -b 127.0.0.1:9092 -P -t my-topic

# Consume messages
kcat -b 127.0.0.1:9092 -C -t my-topic -e

# Consume with consumer group
kcat -b 127.0.0.1:9092 -G my-group my-topic
```

### Produce and Consume with Python

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

## REST API

StrikeMQ includes a built-in REST API on port **8080** — no Kafka tooling required. Inspect broker state, peek at messages, produce, and manage topics with just `curl`.

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/v1/broker` | Broker info (version, uptime, config) |
| GET | `/v1/topics` | List all topics with partition count and offsets |
| GET | `/v1/topics/{name}` | Topic detail with per-partition offsets |
| DELETE | `/v1/topics/{name}` | Delete a topic and its data |
| GET | `/v1/topics/{name}/messages?offset=0&limit=10` | Peek at messages as JSON |
| POST | `/v1/topics/{name}/messages` | Produce messages via JSON body |
| GET | `/v1/groups` | List consumer groups |
| GET | `/v1/groups/{id}` | Group detail with members and committed offsets |

### Examples

```bash
# Broker info
curl localhost:8080/v1/broker
# {"version":"0.1.5","broker_id":0,"uptime_seconds":42,"port":9092,"http_port":8080,...}

# List topics
curl localhost:8080/v1/topics
# [{"name":"my-topic","partitions":1,"start_offset":0,"end_offset":5}]

# Topic detail
curl localhost:8080/v1/topics/my-topic
# {"name":"my-topic","partitions":[{"partition":0,"start_offset":0,"end_offset":5}]}

# Peek at messages
curl "localhost:8080/v1/topics/my-topic/messages?offset=0&limit=3"
# [{"offset":0,"timestamp":1234567890,"key":null,"value":"hello"},...]

# Produce messages
curl -X POST localhost:8080/v1/topics/my-topic/messages \
  -d '{"messages":[{"value":"hello"},{"key":"k1","value":"world"}]}'
# {"topic":"my-topic","partition":0,"base_offset":5,"record_count":2}

# Delete a topic
curl -X DELETE localhost:8080/v1/topics/my-topic
# {"deleted":"my-topic","partitions_removed":1}

# List consumer groups
curl localhost:8080/v1/groups
# [{"group_id":"my-group","state":"Stable","generation_id":1,"member_count":1}]

# Consumer group detail
curl localhost:8080/v1/groups/my-group
# {"group_id":"my-group","state":"Stable","generation_id":1,"protocol_type":"consumer",...}
```

Messages produced via REST are fully compatible with Kafka consumers, and vice versa.

## Performance

| Metric | Value |
|--------|-------|
| Produce latency (p99.9) | < 1ms |
| CPU when idle | 0% |
| Memory footprint | ~1MB + mmap'd segments |
| Startup time | < 10ms |
| Binary size | 52KB |

Run benchmarks: `./build/strikemq_bench`

## Supported Kafka APIs

| API | Versions | Status |
|-----|----------|--------|
| ApiVersions | 0–3 | Full (including flexible versions) |
| Metadata | 0 | Full (auto-topic creation) |
| Produce | 0–5 | Full (persists to disk) |
| Fetch | 0–4 | Full (zero-copy from mmap'd segments) |
| ListOffsets | 0–2 | Full (earliest/latest offset resolution) |
| FindCoordinator | 0–2 | Full (returns self as coordinator) |
| JoinGroup | 0–3 | Full (rebalance protocol, member assignment) |
| SyncGroup | 0–2 | Full (leader distributes partition assignments) |
| Heartbeat | 0–2 | Full (session management, rebalance signaling) |
| LeaveGroup | 0–1 | Full (clean consumer shutdown) |
| OffsetCommit | 0–3 | Full (in-memory offset storage) |
| OffsetFetch | 0–3 | Full (retrieve committed offsets) |
| CreateTopics | 0–3 | Advertised |

## Project Structure

```
├── include/
│   ├── core/types.h              # Broker config, topic types
│   ├── http/http_server.h        # REST API server, JSON writer
│   ├── network/tcp_server.h      # Multi-threaded TCP server (acceptor + workers)
│   ├── network/io_uring_defs.h   # io_uring operation helpers (Linux)
│   ├── protocol/kafka_codec.h    # Kafka encoder/decoder/router
│   ├── storage/partition_log.h   # mmap'd log segments + index
│   ├── storage/consumer_group.h  # Consumer group state management
│   ├── storage/sharded_log_map.h # Sharded topic-partition registry (64 shards)
│   └── utils/                    # Ring buffers, circular buffer, memory pool, endian
├── src/
│   ├── main.cpp                  # Broker orchestration
│   ├── http/http_server.cpp      # REST API handlers + HTTP parser
│   ├── network/tcp_server.cpp    # kqueue/epoll/io_uring implementation
│   └── protocol/kafka_codec.cpp  # Wire protocol codec
├── tests/                        # Unit tests
├── benchmarks/                   # Latency microbenchmarks
└── docs/                         # Architecture, changelog, logo
```

## Running Tests

```bash
cd build && ctest
```

Or individually:

```bash
./strikemq_test_ring              # Lock-free ring buffer
./strikemq_test_pool              # Memory pool allocator
./strikemq_test_codec             # Kafka protocol codec
./strikemq_test_circular_buffer   # Circular I/O buffer
./strikemq_test_sharded_log_map   # Sharded log map
```

## Documentation

- [Architecture](docs/ARCHITECTURE.md) — System design, layer details, platform support
- [Product Guide](docs/PRODUCT.md) — Full feature reference and configuration
- [Changelog](docs/CHANGELOG.md) — Version history and bug fixes

## Limitations (v0.1.5)

StrikeMQ is designed for local development and testing, not production. It trades durability and fault-tolerance for simplicity and speed.

- Consumer group offsets are in-memory only (lost on broker restart)
- No replication (single broker)
- No SASL/SSL authentication
- No log compaction or retention enforcement

## License

MIT
