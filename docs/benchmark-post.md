# StrikeMQ vs Kafka: Benchmarking a 735KB Broker Against a 200MB JVM Giant

Kafka is the gold standard for production event streaming. But for local development and testing, it's like driving a semi truck to the grocery store. I built [StrikeMQ](https://github.com/awneesht/Strike-mq) — a Kafka-compatible broker in C++20 — specifically for the `localhost:9092` use case. Here's how they compare with real numbers.

---

## The Setup

**StrikeMQ v0.1.4** — C++20, zero dependencies, single binary.
**Apache Kafka 3.7** — Running via `docker compose` with KRaft (no ZooKeeper), default configuration.
**Hardware** — Apple M-series MacBook, 10 cores, 16GB RAM.

All tests measure the same thing: a process listening on port 9092 that Kafka clients can produce to and consume from.

---

## Binary Size

| | Kafka | StrikeMQ |
|---|---:|---:|
| Runtime files | ~200MB (JVM + jars + config) | **735KB** (stripped, statically linked) |
| Dependencies | JDK 11+, scripts, config dirs | None |

StrikeMQ is **272x smaller**. The entire binary — networking, Kafka protocol codec, storage engine, REST API, HTTP server — fits in less space than a single JPEG.

```
$ ls -lh strikemq
-rwxr-xr-x  1 user  staff  735K  strikemq

$ du -sh kafka_2.13-3.7.0/
207M    kafka_2.13-3.7.0/
```

---

## Startup Time

I measured time from process start to first successful produce (using `kcat`):

| | Kafka | StrikeMQ |
|---|---:|---:|
| Cold start to ready | ~8-15 seconds | **< 10ms** |
| First produce accepted | ~10-20 seconds | **< 50ms** |

```bash
# StrikeMQ: instant
$ time (./strikemq & sleep 0.1 && echo "test" | kcat -b 127.0.0.1:9092 -P -t bench)
real    0m0.112s

# Kafka: wait for JVM warmup, controller election, log recovery...
$ time (docker compose up -d && until kcat -b 127.0.0.1:9092 -L 2>/dev/null; do sleep 0.5; done)
real    0m12.438s
```

When you're iterating on code and restarting your broker 50 times a day, those 12 seconds add up to **10 minutes of daily waiting**.

---

## Memory Usage

Measured after startup with no topics, then after producing 10,000 messages:

| State | Kafka | StrikeMQ |
|---|---:|---:|
| Idle (no topics) | ~350MB RSS | **~1.5MB** RSS |
| After 10K messages | ~400MB RSS | **~2MB** + mmap'd segments |
| Theoretical minimum | ~200MB (JVM heap floor) | **< 1MB** (code + stack) |

StrikeMQ uses `mmap` for storage segments. The OS manages page residency — only pages being read or written are in physical memory. The broker itself barely allocates heap. Kafka, by contrast, needs a JVM with a minimum heap, GC metadata, thread stacks for 50+ threads, and page cache for its own log segments.

---

## Idle CPU

| | Kafka | StrikeMQ |
|---|---:|---:|
| CPU at idle | 1-3% (GC cycles, thread scheduling) | **0.0%** |

StrikeMQ uses `kqueue` (macOS) / `epoll` (Linux) event loops that block when there's nothing to do. No background GC, no periodic timers, no busy loops. The process is literally suspended by the kernel until a packet arrives.

```bash
# StrikeMQ idle for 60 seconds
$ top -pid $(pgrep strikemq) -l 1
PID    COMMAND  %CPU  MEM
12345  strikemq  0.0   1.5M
```

---

## Produce Latency — Microbenchmarks

StrikeMQ's built-in benchmark suite measures the raw latency of core operations using TSC (Time Stamp Counter) for nanosecond-precision timing. 1 million samples each after a 10K warmup:

### SPSC Ring Buffer (push + pop)
The lock-free queue that passes connections from the acceptor thread to workers:

| Percentile | Latency |
|---:|---:|
| avg | **19 ns** |
| p50 | < 42 ns |
| p99.9 | **42 ns** |
| max | 13 us |

### Memory Pool (alloc + free)
Pre-allocated block pool with intrusive freelist:

| Percentile | Latency |
|---:|---:|
| avg | **3 ns** |
| p50 | < 42 ns |
| p99.9 | **42 ns** |
| max | 7 us |

### Log Append (1KB message)
The full produce path — lock partition, memcpy into mmap'd segment, update offset index, unlock:

| Percentile | Latency |
|---:|---:|
| avg | **145 ns** |
| p50 | **83 ns** |
| p99 | **667 ns** |
| p99.9 | **4.4 us** |
| max | 15 us |

### Kafka Header Decode
Parsing a complete Kafka request header from raw bytes:

| Percentile | Latency |
|---:|---:|
| avg | **16 ns** |
| p50 | < 42 ns |
| p99.9 | **42 ns** |
| max | 15 us |

**Every operation passes the sub-millisecond p99.9 check.** The log append — which is the actual disk write — completes in 145ns on average. That's because `mmap` turns disk writes into memory copies; the OS flushes to disk asynchronously.

---

## End-to-End Produce Latency

For the full network round-trip (client -> TCP -> parse -> store -> respond -> client), measured with `kcat` producing 1,000 individual messages:

| | Kafka | StrikeMQ |
|---|---:|---:|
| p50 | ~1-2ms | **< 0.5ms** |
| p99 | ~5-10ms | **< 1ms** |
| p99.9 | ~15-50ms | **< 1ms** |

StrikeMQ's end-to-end produce stays under 1ms at p99.9. The path is:
```
recv() → parse Kafka header (16ns) → decode batch → lock partition mutex →
memcpy into mmap (145ns) → unlock → encode response → send()
```

No GC pauses. No thread context switches for common cases. No JIT warmup.

---

## Consume Latency

The fetch path is even faster because it's completely lock-free:

```
recv() → parse header → binary search offset index → pointer into mmap → send()
```

Zero copies of actual message data. The kernel's `send()` reads directly from the mmap'd file pages. No deserialization, no buffer allocation, no locking.

---

## Resource Comparison Summary

| Metric | Kafka | StrikeMQ | Factor |
|--------|------:|--------:|-------:|
| Binary size | 200MB | 735KB | **272x** smaller |
| Startup time | 12s | 10ms | **1,200x** faster |
| Idle memory | 350MB | 1.5MB | **233x** less |
| Idle CPU | 1-3% | 0% | -- |
| Produce p99.9 | ~15ms | < 1ms | **15x+** faster |
| Dependencies | JDK, scripts | None | -- |
| Threads at idle | 50+ | 12 | **4x** fewer |

---

## What This Means For You

If you're running Kafka in `docker-compose.yml` for local development, you're paying a **12-second startup tax** and **350MB memory overhead** every time. Multiply that across your team and your CI pipeline:

- **Developer laptop:** Swap Kafka for StrikeMQ in docker-compose. Same port, same protocol, same client code. Free up 350MB for your IDE.
- **CI/CD integration tests:** Start StrikeMQ in 10ms instead of waiting 15 seconds for Kafka to boot. Your pipeline gets faster without changing a single test.
- **Prototyping:** Want to test if Kafka is right for your architecture? Try the idea with StrikeMQ in seconds, not minutes.

---

## What StrikeMQ Doesn't Do

This isn't a production Kafka replacement. It deliberately trades durability and fault tolerance for speed and simplicity:

- No replication (single broker)
- No authentication (no SASL/SSL)
- Consumer group offsets are in-memory (lost on restart)
- No log compaction or retention enforcement

It's a **development tool**, like SQLite is to PostgreSQL or LocalStack is to AWS.

---

## Try It

```bash
# macOS
brew tap awneesht/strike-mq
brew install strikemq

# Or build from source (any platform)
git clone https://github.com/awneesht/Strike-mq.git
cd Strike-mq && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/strikemq
```

Then point any Kafka client at `127.0.0.1:9092`. Or use the built-in REST API:

```bash
# Produce via curl
curl -X POST localhost:8080/v1/topics/demo/messages \
  -d '{"messages":[{"value":"hello"},{"key":"user-1","value":"world"}]}'

# Peek at messages
curl "localhost:8080/v1/topics/demo/messages?offset=0&limit=10"
```

Run the benchmarks yourself:

```bash
./build/strikemq_bench
```

**GitHub:** [github.com/awneesht/Strike-mq](https://github.com/awneesht/Strike-mq)
**License:** MIT

---

*All benchmarks run on Apple M-series, macOS, compiled with Clang -O2. Your numbers will vary. Kafka numbers are representative of default configurations — tuned Kafka will perform better, but will still carry the JVM baseline overhead. StrikeMQ numbers are from its built-in benchmark suite using TSC-based nanosecond timing.*
