# I Replaced a 200MB JVM Process with a 52KB Binary That Speaks Kafka

Every time I spin up Kafka for local development, the same ritual plays out: start ZooKeeper (or KRaft), wait for the JVM to warm up, watch 2GB of RAM disappear, and then finally — after 30 seconds — send my first message.

I got tired of it. So I built **StrikeMQ** — a 52KB message broker written in C++20 that speaks the Kafka wire protocol. Any Kafka client library works with it out of the box. No code changes. No JVM. No ZooKeeper. Start in milliseconds, 0% CPU when idle.

Think of it like [LocalStack](https://localstack.cloud/) for Kafka — develop locally against StrikeMQ, deploy to real Kafka in production.

```bash
# Build
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .

# Run
./strikemq

# Produce and consume with any Kafka client
echo -e "hello\nworld\nstrike" | kcat -b 127.0.0.1:9092 -P -t my-topic
kcat -b 127.0.0.1:9092 -C -t my-topic -e
```

This post is about how I built it, the bugs that nearly broke me, and what I learned about implementing a real wire protocol from scratch.

---

## Why Not Just Use Kafka?

Kafka is incredible for production. But for local development and testing, it's overkill:

| | Kafka | StrikeMQ |
|---|---|---|
| Binary size | ~200MB+ (JVM + libs) | 52KB |
| Startup time | 10-30 seconds | < 10ms |
| Idle CPU | 1-5% (JVM GC, threads) | 0% |
| Memory | 1-2GB minimum | ~1MB + mmap'd segments |
| Dependencies | JVM, ZooKeeper/KRaft | None |

I didn't want to build a Kafka replacement. I wanted something that _pretends_ to be Kafka well enough that `kafka-python`, `librdkafka`, `kcat`, and `confluent-kafka-go` can't tell the difference.

---

## The Architecture

StrikeMQ has four layers, all in pure C++20 with zero third-party dependencies:

```
        Kafka Clients (kcat, librdkafka, kafka-python, ...)
                            |
                       TCP :9092
                            |
              +----------------------------+
              |     Acceptor Thread        |
              |  kqueue/epoll (accept only)|
              +----------------------------+
                  |     |     |        |
            SPSC ring buffers (lock-free)
                  |     |     |        |
          Worker 0  Worker 1  ...  Worker N-1
          (own kqueue/epoll/io_uring per thread)
                  |     |     |        |
              +----------------------------+
              |      Protocol Layer        |
              |  Kafka wire protocol       |
              |  encode/decode/route       |
              +----------------------------+
                    |     |     |     |
              Produce  Fetch  List   Metadata
                              Offsets
                    |     |
              +----------------------------+
              |   Consumer Group Handlers  |
              |  JoinGroup, SyncGroup,     |
              |  Heartbeat, OffsetCommit   |
              +----------------------------+
                    |     |
              +----------------------------+
              |      Storage Layer         |
              |  ShardedLogMap<64>         |
              |  mmap'd log segments       |
              |  sparse offset index       |
              |  (per-shard + per-partition |
              |   mutex)                   |
              +----------------------------+
                            |
                    /tmp/strikemq/data/
```

### Multi-Threaded I/O

The network layer uses an **acceptor + N worker threads** architecture. The acceptor thread runs its own `kqueue` (macOS) or `epoll` (Linux) loop that does nothing but `accept()` new connections and distribute them round-robin to worker threads via lock-free SPSC ring buffers. Each worker thread runs its own event loop with its own `kqueue`/`epoll`/`io_uring` instance, its own connection map, and a pipe-based wakeup mechanism for cross-thread notification.

On Linux with `STRIKE_IO_URING`, workers use **io_uring** instead of epoll — submission-based `recv`/`send` with SQPOLL for kernel-side submission. This eliminates per-I/O syscall overhead entirely.

N defaults to `std::thread::hardware_concurrency()` — on a 10-core machine, that's 10 independent event loops processing requests in parallel. A slow consumer fetch on worker 3 no longer blocks a fast producer on worker 7.

Every socket gets `TCP_NODELAY` for minimum latency, and each worker processes up to 64 events per iteration with **1ms timeouts**. Connection I/O uses `FixedCircularBuffer` (128KB, power-of-2 ring buffer) instead of `std::vector` — O(1) advance instead of erase+realloc after every `send()`. Frame extraction happens inline — we read the 4-byte big-endian size prefix, accumulate bytes until a full Kafka frame arrives, then route it to the protocol layer. Connection state is thread-local to each worker, so there's no locking on the I/O hot path.

### Zero-Copy Storage

Messages are stored in memory-mapped log segments, pre-allocated to 1GB each. Writes are sequential `memcpy` into the mapped region. Reads are zero-copy — the Fetch handler returns a raw pointer directly into the mmap'd segment. No serialization, no copying, no allocation on the read path.

```
/tmp/strikemq/data/
  my-topic-0/
    0.log         # 1GB pre-allocated, mmap'd
  another-topic-0/
    0.log
```

A sparse offset index (one entry per 4KB boundary) maps logical Kafka offsets to byte positions. Lookups use `std::lower_bound` for O(log n) performance, then scan forward through batch headers to find the exact starting position.

### Lock-Free Data Structures

The lock-free primitives aren't theoretical — they're load-bearing infrastructure for the multi-threaded architecture:

- **SPSC ring buffer** — Used to pass accepted file descriptors from the acceptor thread to each worker. Wait-free, cache-line aligned (64 bytes) to prevent false sharing. Uses separate cached head/tail copies to minimize cross-core cache traffic. One ring buffer per worker (acceptor = producer, worker = consumer), so no contention between workers.
- **MPSC ring buffer** — Compare-and-swap loop for multi-producer safety with a committed flag per slot.
- **Memory pool** — Pre-allocated block pool with an intrusive freelist. On Linux, it tries `MAP_HUGETLB` for 2MB pages, with automatic fallback to regular pages. The constructor touches every page to force materialization and prevent page faults on the hot path.

Storage is managed by `ShardedLogMap<64>` — 64 cache-line-aligned shards, each with its own mutex and map. A topic-partition hashes to one of 64 shards, so concurrent operations to different shards never contend. Within a shard, `PartitionLog::append()` holds a per-partition mutex, so even within the same shard, writes to different partitions are independent. The read path (`PartitionLog::read()`) is completely lock-free, using only acquire loads on atomics to see committed data.

---

## Implementing the Kafka Wire Protocol

This is where it gets interesting. The Kafka protocol is a binary, big-endian, version-aware request/response protocol over TCP. Every request starts with:

```
[4 bytes] message size
[2 bytes] API key (which operation)
[2 bytes] API version
[4 bytes] correlation ID
[variable] client ID string
```

I implemented five core APIs:

| API | What It Does |
|-----|-------------|
| **ApiVersions** | "What do you support?" — Client's first request |
| **Metadata** | "What topics exist? Where are the brokers?" |
| **Produce** | "Store these messages" |
| **Fetch** | "Give me messages starting from offset X" |
| **ListOffsets** | "What's the earliest/latest offset for this partition?" |

Each API has multiple versions with different field layouts. Produce alone has versions 0 through 5, each adding fields like `transactional_id` or changing how `acks` works. The encoder and decoder are version-aware — they check the API version and include/skip fields accordingly.

---

## The Bugs That Nearly Broke Me

### Bug #1: The ~34GB Malloc

When I first connected `librdkafka`, the broker crashed immediately. Not a segfault in my code — a `malloc` assertion failure _inside librdkafka_.

Here's what happened: librdkafka sends `ApiVersions v3`, which uses Kafka's "flexible versions" encoding. This means compact arrays (varint-prefixed instead of int32-prefixed) and tagged fields at the end of each section.

My encoder dutifully added a `tagged_fields` byte (0x00 = no tags) to the response header. But the Kafka protocol spec has a **special exception**: ApiVersions responses must NOT include header tagged fields, for backwards compatibility with older clients.

That one extra byte shifted every subsequent field by 1 position. When librdkafka parsed the "number of API entries" field, it read a garbage value that translated to approximately **34 billion entries**. It tried to malloc ~34GB, the allocator returned NULL, and the process aborted.

**The fix:** One line removed — don't write the header tagged_fields byte for ApiVersions responses.

### Bug #2: The INT16 That Was an INT32

After implementing Fetch, `kcat` connected and tried to consume messages. Instead of data, I got:

```
rd_kafka_msgset_reader_msg_v2:764: expected 18446744073709551613 bytes
```

That number is `(uint64_t)-3` — a clear sign that a signed value was being interpreted as unsigned, and something was off by a few bytes in the binary layout.

The Kafka v2 record batch header has 49 bytes of fixed fields. Two of them — `attributes` and `producerEpoch` — are **INT16** (2 bytes each). But my serializer was writing them as INT32 (4 bytes each):

```cpp
// BEFORE (broken):
w32(batch.attributes);      // wrote 4 bytes, should be 2
w32(batch.producer_epoch);  // wrote 4 bytes, should be 2

// AFTER (fixed):
w16(batch.attributes);      // correct: 2 bytes
w16(batch.producer_epoch);  // correct: 2 bytes
```

Those 4 extra bytes shifted every record in the batch. When librdkafka parsed with the correct field sizes, the varint decoder landed on garbage bytes and produced nonsensical lengths.

This bug was particularly nasty because produces appeared to succeed — the broker accepted and stored the data. It only manifested on consume, when a client tried to parse the stored bytes with the correct field widths.

**How I found it:** I wrote a Python script to hex-dump the raw `.log` file and manually walked through each field of the Kafka v2 batch format, byte by byte, until I found the offset where reality diverged from the spec.

### Bug #3: librdkafka's Version Gate

Even after fixing the serialization, `kcat` refused to parse the response. Debug logs showed:

```
Feature MsgVer2: Fetch (4..32767) NOT supported by broker
```

librdkafka has a **feature gate**: it only uses Kafka v2 record batches if the broker advertises Fetch v4 or higher. I was advertising Fetch v0-v0 — valid, but insufficient. The client fell back to an older message format that didn't match what was stored on disk.

**The fix:** Advertise Fetch v0-v4 and update the response encoder to handle v1+ fields (`throttle_time_ms`) and v4+ fields (`last_stable_offset`, `aborted_transactions`).

---

## Performance

On my M-series MacBook (measured with `strikemq_bench`, v0.1.5):

| Metric | Value |
|--------|-------|
| Log append 1KB (p50) | 83 ns |
| Log append 1KB (p99.9) | 4.4 μs |
| SPSC ring push+pop (p99.9) | 42 ns |
| Kafka header decode (p99.9) | 42 ns |
| CPU when idle | 0% |
| Memory footprint | ~1MB + mmap'd segments |
| Startup time | < 10ms |
| Binary size | 52KB |

The produce path is: recv() → parse header → decode batch → shard lookup (1/64) → lock partition mutex → memcpy into mmap → unlock → encode response → send(). Shard selection is a hash + bitmask, so writes to different topic-partitions across shards are fully parallel across worker threads with zero contention.

The consume path is even simpler: recv() → parse header → binary search the offset index → return a pointer into the mmap'd segment → send(). Zero copies of the actual message data, and completely lock-free.

---

## What Works Today

```python
from kafka import KafkaProducer, KafkaConsumer

# Produce
producer = KafkaProducer(bootstrap_servers='127.0.0.1:9092')
producer.send('my-topic', b'hello from python')
producer.flush()

# Consume
consumer = KafkaConsumer('my-topic',
                         bootstrap_servers='127.0.0.1:9092',
                         auto_offset_reset='earliest')
for msg in consumer:
    print(msg.value.decode())  # "hello from python"
    break

# Consume with consumer group
consumer = KafkaConsumer('my-topic', group_id='my-group',
                         bootstrap_servers='127.0.0.1:9092',
                         auto_offset_reset='earliest')
for msg in consumer:
    print(msg.value.decode())
    break
```

Supported APIs: ApiVersions (v0-v3), Metadata (v0), Produce (v0-v5), Fetch (v0-v4), ListOffsets (v0-v2), FindCoordinator (v0-v2), JoinGroup (v0-v3), SyncGroup (v0-v2), Heartbeat (v0-v2), LeaveGroup (v0-v1), OffsetCommit (v0-v3), OffsetFetch (v0-v3). Topics are auto-created on first produce or metadata request.

---

## What's Next

- **Log compaction and retention** — Segments accumulate indefinitely right now

---

## Try It

StrikeMQ is MIT licensed and runs on macOS (Apple Silicon + Intel) and Linux.

**GitHub:** [github.com/awneesht/Strike-mq](https://github.com/awneesht/Strike-mq)

**Docker (easiest):**

```bash
docker run -p 9092:9092 -p 8080:8080 awneesh/strikemq
```

**Or build from source:**

```bash
git clone https://github.com/awneesht/Strike-mq.git
cd Strike-mq
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
./strikemq
```

If you're tired of waiting 30 seconds for Kafka to start during local development, give it a try. Stars and feedback welcome.
