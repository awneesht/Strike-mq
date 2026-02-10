# Changelog

## v0.1.2 — Consumer Group Protocol

### New Features

- **Kafka Consumer Group Protocol** — Full implementation of all 7 consumer group APIs, enabling `kcat -G` and `kafka-python` with `group_id`. Consumers can now coordinate partition assignment, commit offsets, and maintain group membership.

  - **FindCoordinator (API 10, v0-v2)** — Returns self as coordinator (single broker)
  - **JoinGroup (API 11, v0-v3)** — Member ID generation, rebalance triggering, leader election, protocol selection by vote
  - **SyncGroup (API 14, v0-v2)** — Leader distributes partition assignments to all members
  - **Heartbeat (API 12, v0-v2)** — Session management with lazy timeout checks, REBALANCE_IN_PROGRESS signaling
  - **LeaveGroup (API 13, v0-v1)** — Clean consumer shutdown with rebalance on departure
  - **OffsetCommit (API 8, v0-v3)** — In-memory per-(group, topic, partition) offset storage
  - **OffsetFetch (API 9, v0-v3)** — Retrieves committed offsets, returns -1 for uncommitted partitions

- **ConsumerGroupManager** — New header-only class (`include/storage/consumer_group.h`) managing group state machine (Empty → PreparingRebalance → CompletingRebalance → Stable), session timeouts, and committed offsets in memory

- **StringHash** — FNV-1a string hasher added to `types.h` to avoid libc++ ABI issues with `std::hash<std::string>` on Homebrew Clang

### Design Decisions

- **In-memory group state** — No `__consumer_offsets` topic; offsets are lost on restart (acceptable for dev/test)
- **Immediate rebalance** — No delay timer; generation increments immediately on join/leave
- **Lazy session timeouts** — Checked during heartbeat calls rather than via background threads
- **Single broker = coordinator** — FindCoordinator always returns self

### Files Changed

| File | Change |
|------|--------|
| `include/core/types.h` | Added 11 consumer group request/response structs, `StringHash` |
| `include/protocol/kafka_codec.h` | Added `LeaveGroup` to ApiKey, 4 error codes, 7 decoder/encoder declarations, 7 handler types |
| `include/storage/consumer_group.h` | **New** — ConsumerGroupManager with group state machine |
| `src/protocol/kafka_codec.cpp` | 7 decoders, 7 encoders, 7 router switch cases |
| `src/main.cpp` | ConsumerGroupManager instance, 7 handler lambdas |

### Verified With

- `kcat -b 127.0.0.1:9092 -G test-group test-topic` — Consumer group join, assign, fetch, leave
- `kcat -X auto.offset.reset=earliest -G test-group test-topic` — Reads pre-existing messages
- Produce-while-consuming — Messages delivered in real-time to group consumer
- All existing unit tests pass (ring buffer, memory pool, codec)

---

## v0.1.1 — Fetch (Consume) & ListOffsets APIs

### New Features

- **Kafka Fetch API (consume)** — Full implementation of Fetch v0-v4. Kafka clients can now consume messages from BlazeMQ using standard consumer APIs. Uses zero-copy reads directly from mmap'd log segments.
  - `encode_fetch_response()` — Version-aware encoder: v0 basic, v1+ adds `throttle_time_ms`, v4+ adds `last_stable_offset` and `aborted_transactions`
  - `PartitionLog::read()` — Scans segment batch headers via sparse offset index, returns raw pointer + size into mmap'd data
  - `ReadResult` struct for zero-copy return values
  - `FetchPartitionResponse` struct with mmap pointer fields

- **Kafka ListOffsets API** — Full implementation of ListOffsets v0-v2. Resolves logical offsets (earliest=-2, latest=-1) to actual offset numbers, required by clients before fetching.
  - `decode_list_offsets()` — Parses v0-v2 requests with topic-partition arrays and timestamps
  - `encode_list_offsets_response()` — v0 returns offset array, v1+ returns single offset+timestamp, v2+ adds `throttle_time_ms`

### Bug Fixes

#### Record Batch Serialization Field Width (Critical)

**Problem:** `serialize_batch()` in `partition_log.h` wrote the `attributes` field (INT16) and `producer_epoch` field (INT16) as 4-byte INT32 values using `w32()`. This added 4 extra bytes to the batch header, shifting all record data. When librdkafka parsed the batch with correct field sizes, every field after `attributes` was misaligned, causing the varint parser to read garbage values (e.g., `expected 18446744073709551613 bytes`).

**Fix:** Added `w16()` lambda for 2-byte writes and used it for `attributes` and `producer_epoch` fields.

#### Record Byte Range Tracking in Decoder

**Problem:** `decode_record_batch()` set `Record.data = nullptr` and calculated `total_size` as only `key_length + value_length`, missing the varint overhead bytes. When re-serializing records for Fetch responses, the byte range was incomplete.

**Fix:** Track `rec_start = r.current()` before parsing each record and set `rec.data` / `rec.total_size` to the full byte range including all varint headers.

### Files Changed

| File | Change |
|------|--------|
| `include/core/types.h` | Added `FetchPartitionResponse`, `ListOffsetsRequest`, `ListOffsetsPartitionResponse` structs |
| `include/storage/partition_log.h` | Added `ReadResult` struct, implemented `read()` with batch scanning, fixed `serialize_batch()` INT16 fields |
| `include/protocol/kafka_codec.h` | Added `encode_fetch_response()`, `encode_list_offsets_response()`, `decode_list_offsets()`, `write_raw()`, updated `FetchHandler` typedef, added `ListOffsetsHandler` |
| `src/protocol/kafka_codec.cpp` | Implemented Fetch/ListOffsets encode/decode, added router cases, fixed `decode_record_batch()` byte tracking |
| `src/main.cpp` | Registered Fetch and ListOffsets handlers |

### Verified With

- `echo "hello" | kcat -b 127.0.0.1:9092 -P -t test-topic` — Produces messages
- `kcat -b 127.0.0.1:9092 -C -t test-topic -e` — Consumes all messages and exits
- Multi-message produce/consume — Correct ordering verified
- Python `kafka-python` KafkaConsumer — Reads messages from earliest offset
- All unit tests pass (ring buffer, memory pool, codec)

---

## v0.1.0 — TCP Networking & Kafka Client Compatibility

### New Features

- **TCP server with kqueue/epoll event loop** — Non-blocking, event-driven networking replaces the busy-spin placeholder. 0% CPU when idle.
  - `include/network/tcp_server.h` — TcpServer class with per-connection buffering
  - `src/network/tcp_server.cpp` — Platform-specific implementation (kqueue on macOS, epoll on Linux)

- **Full request routing** — `main.cpp` rewritten to wire TcpServer, RequestRouter, and PartitionLog together with produce and metadata handlers.

- **Metadata handler with auto-topic creation** — Topics are automatically created when requested via Metadata API, matching Kafka's `auto.create.topics.enable` behavior.

- **MetadataHandler callback** added to `RequestRouter` — Accepts requested topic names and returns broker/topic info.

### Bug Fixes

#### ApiVersions v3 Flexible Encoding (Critical)

**Problem:** librdkafka v2.13.0 sends `ApiVersions v3` which uses Kafka's "flexible versions" protocol (compact arrays, tagged fields). Our response included a header `tagged_fields` byte, but the Kafka protocol spec has a special exception: ApiVersions responses must **not** include header tagged_fields for backwards compatibility. This extra byte shifted all subsequent fields by 1, causing librdkafka to parse the compact array length as a garbage value (~34GB), triggering a `malloc` assertion crash (`rd_malloc` returned NULL).

**Fix:** Removed the header `tagged_fields` byte from ApiVersions v3 responses. Added version-aware encoding: v0-v2 uses standard format, v3+ uses compact arrays and per-entry tagged fields but no header tags.

**Root cause location:** `src/protocol/kafka_codec.cpp` — `encode_api_versions_response()`.

#### API Version Maximums Too High

**Problem:** The ApiVersions response advertised support for Metadata v0-v12, Produce v0-v9, etc. librdkafka would use the highest advertised version, but our encoder only produced v0 format responses. Metadata v5+ adds fields like `throttle_time_ms`, `rack`, `controller_id`, `is_internal`, and `offline_replicas` that we don't encode, causing "Partial response" parse errors.

**Fix:** Lowered advertised API version maximums to match what the encoder actually implements. Metadata is now v0 only. Added a full set of 13 consumer-group and admin APIs (FindCoordinator, JoinGroup, Heartbeat, LeaveGroup, SyncGroup, CreateTopics) to the ApiVersions response to satisfy librdkafka's feature detection.

#### IPv6 Connection Failure

**Problem:** The Metadata response returned `localhost` as the broker host. librdkafka resolved `localhost` to IPv6 `[::1]` first, but the broker only binds to IPv4 `0.0.0.0:9092`. Produce requests failed with "Connection refused" on the IPv6 address.

**Fix:** Changed the Metadata response to return `127.0.0.1` instead of `localhost` when the bind address is `0.0.0.0`.

#### Topic Auto-Creation Missing

**Problem:** kcat sends a Metadata request for the target topic before producing. Our Metadata handler only returned already-known topics (empty list for new topics). kcat waited indefinitely for the topic to appear in metadata, never sending the actual produce request.

**Fix:** The Metadata handler now auto-creates requested topics by calling `get_or_create_log()` for each topic in the Metadata request, then returns them in the response.

#### stdout Buffering

**Problem:** The startup banner was buffered and not visible when running the broker.

**Fix:** Added `std::flush` after the banner and all log output.

#### TopicPartitionHash libc++ ABI Issue

**Problem:** The `TopicPartitionHash` struct used `std::hash<std::string>` which, under Homebrew LLVM's clang, generates calls to `std::__1::__hash_memory()` — a symbol present in LLVM's libc++ but not in Apple's system libc++. This caused a linker error: "Undefined symbols: std::__1::__hash_memory".

**Fix:** Replaced `std::hash<std::string>` with a manual FNV-1a hash implementation that has no libc++ dependency.

### Files Changed

| File | Change |
|------|--------|
| `include/network/tcp_server.h` | **New** — TcpServer class |
| `src/network/tcp_server.cpp` | **New** — kqueue/epoll implementation |
| `src/main.cpp` | **Rewritten** — Broker orchestration with handlers |
| `include/protocol/kafka_codec.h` | Added MetadataInfo struct, MetadataHandler, api_version param |
| `src/protocol/kafka_codec.cpp` | v3 ApiVersions encoding, Metadata routing, version-aware dispatch |
| `include/core/types.h` | FNV-1a hash for TopicPartitionHash |
| `tests/kafka_codec_test.cpp` | Updated test for new api_version parameter |

### Verified With

- `kcat -b 127.0.0.1:9092 -L` — Lists broker and topics
- `echo "hello" | kcat -b 127.0.0.1:9092 -P -t test-topic` — Produces messages
- `lsof -i :9092` — Confirms listening socket
- `ps aux` — Confirms 0% CPU when idle
- Python raw socket test — Validates ApiVersions v0 and v3 response formats
