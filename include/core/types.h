#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace strike {

using Timestamp = uint64_t;

struct TopicPartition {
    std::string topic;
    int32_t partition;
    bool operator==(const TopicPartition& o) const {
        return partition == o.partition && topic == o.topic;
    }
};

struct TopicPartitionHash {
    size_t operator()(const TopicPartition& tp) const {
        // FNV-1a to avoid std::hash<std::string> libc++ ABI issues
        size_t h = 14695981039346656037ULL;
        for (char c : tp.topic) {
            h ^= static_cast<size_t>(static_cast<unsigned char>(c));
            h *= 1099511628211ULL;
        }
        h ^= static_cast<size_t>(static_cast<uint32_t>(tp.partition));
        h *= 1099511628211ULL;
        return h;
    }
};

struct StringHash {
    size_t operator()(const std::string& s) const {
        size_t h = 14695981039346656037ULL;
        for (char c : s) {
            h ^= static_cast<size_t>(static_cast<unsigned char>(c));
            h *= 1099511628211ULL;
        }
        return h;
    }
};

struct Record {
    int64_t offset = -1;
    int64_t timestamp = 0;
    int8_t timestamp_type = 0;
    uint8_t* data = nullptr;
    uint32_t key_offset = 0;
    uint32_t key_length = 0;
    uint32_t value_offset = 0;
    uint32_t value_length = 0;
    uint32_t total_size = 0;

    [[nodiscard]] std::string_view key() const {
        if (key_length == 0) return {};
        return {reinterpret_cast<const char*>(data + key_offset), key_length};
    }
    [[nodiscard]] std::string_view value() const {
        return {reinterpret_cast<const char*>(data + value_offset), value_length};
    }
};

struct RecordBatch {
    TopicPartition topic_partition;
    int64_t base_offset = -1;
    int64_t last_offset = -1;
    int32_t partition_leader_epoch = 0;
    int8_t magic = 2;
    int32_t crc = 0;
    int16_t attributes = 0;
    int64_t first_timestamp = 0;
    int64_t max_timestamp = 0;
    int64_t producer_id = -1;
    int16_t producer_epoch = -1;
    int32_t base_sequence = -1;
    std::vector<Record> records;

    [[nodiscard]] uint32_t serialized_size() const {
        uint32_t size = 61;
        for (const auto& r : records) size += r.total_size;
        return size;
    }
};

struct ProduceRequest {
    int32_t correlation_id = 0;
    std::string client_id;
    int16_t acks = -1;
    int32_t timeout_ms = 30000;
    std::vector<RecordBatch> batches;
};

struct ProducePartitionResponse {
    int32_t partition = 0;
    int16_t error_code = 0;
    int64_t base_offset = 0;
    int64_t log_append_time = -1;
    int64_t log_start_offset = 0;
};

struct FetchPartitionResponse {
    TopicPartition tp;
    int16_t error_code = 0;
    int64_t high_watermark = -1;
    const uint8_t* record_data = nullptr;
    uint32_t record_data_size = 0;
};

struct FetchRequest {
    int32_t correlation_id = 0;
    std::string client_id;
    int32_t max_wait_ms = 500;
    int32_t min_bytes = 1;
    int32_t max_bytes = 52428800;
    struct PartitionFetch {
        TopicPartition tp;
        int64_t fetch_offset = 0;
        int32_t partition_max_bytes = 1048576;
    };
    std::vector<PartitionFetch> partitions;
};

struct ListOffsetsRequest {
    int32_t correlation_id = 0;
    int16_t api_version = 0;
    struct PartitionRequest {
        TopicPartition tp;
        int64_t timestamp = -1; // -1 = latest, -2 = earliest
    };
    std::vector<PartitionRequest> partitions;
};

struct ListOffsetsPartitionResponse {
    TopicPartition tp;
    int16_t error_code = 0;
    int64_t timestamp = -1;
    int64_t offset = 0;
};

// ---- Consumer Group API types ----

struct FindCoordinatorRequest {
    int32_t correlation_id = 0;
    int16_t api_version = 0;
    std::string group_id;
    int8_t coordinator_type = 0; // 0 = group, 1 = transaction (v1+)
};

struct JoinGroupProtocol {
    std::string name;
    std::vector<uint8_t> metadata;
};

struct JoinGroupRequest {
    int32_t correlation_id = 0;
    int16_t api_version = 0;
    std::string client_id;
    std::string group_id;
    int32_t session_timeout_ms = 10000;
    int32_t rebalance_timeout_ms = 300000; // v1+
    std::string member_id;
    std::string protocol_type;
    std::vector<JoinGroupProtocol> protocols;
};

struct JoinGroupMember {
    std::string member_id;
    std::vector<uint8_t> metadata;
};

struct JoinGroupResponse {
    int16_t error_code = 0;
    int32_t generation_id = -1;
    std::string protocol_name;
    std::string leader_id;
    std::string member_id;
    std::vector<JoinGroupMember> members;
};

struct SyncGroupAssignment {
    std::string member_id;
    std::vector<uint8_t> assignment;
};

struct SyncGroupRequest {
    int32_t correlation_id = 0;
    int16_t api_version = 0;
    std::string group_id;
    int32_t generation_id = -1;
    std::string member_id;
    std::vector<SyncGroupAssignment> assignments;
};

struct SyncGroupResponse {
    int16_t error_code = 0;
    std::vector<uint8_t> member_assignment;
};

struct HeartbeatRequest {
    int32_t correlation_id = 0;
    int16_t api_version = 0;
    std::string group_id;
    int32_t generation_id = -1;
    std::string member_id;
};

struct LeaveGroupRequest {
    int32_t correlation_id = 0;
    int16_t api_version = 0;
    std::string group_id;
    std::string member_id;
};

struct OffsetCommitPartitionRequest {
    TopicPartition tp;
    int64_t offset = 0;
    int64_t timestamp = -1; // v1 only
    std::string metadata;
};

struct OffsetCommitRequest {
    int32_t correlation_id = 0;
    int16_t api_version = 0;
    std::string group_id;
    int32_t generation_id = -1;   // v1+
    std::string member_id;        // v1+
    int64_t retention_time_ms = -1; // v2+
    std::vector<OffsetCommitPartitionRequest> partitions;
};

struct OffsetCommitPartitionResponse {
    TopicPartition tp;
    int16_t error_code = 0;
};

struct OffsetFetchPartitionRequest {
    TopicPartition tp;
};

struct OffsetFetchRequest {
    int32_t correlation_id = 0;
    int16_t api_version = 0;
    std::string group_id;
    std::vector<OffsetFetchPartitionRequest> partitions;
};

struct OffsetFetchPartitionResponse {
    TopicPartition tp;
    int64_t offset = -1;
    std::string metadata;
    int16_t error_code = 0;
};

struct BrokerConfig {
    std::string bind_address = "0.0.0.0";
    uint16_t port = 9092;
    uint32_t num_io_threads = 0;
    bool use_dpdk = false;
    int dpdk_port_id = 0;
    uint16_t dpdk_rx_queues = 4;
    uint16_t dpdk_tx_queues = 4;
    uint32_t dpdk_rx_ring_size = 4096;
    uint32_t dpdk_tx_ring_size = 4096;
    std::string log_dir = "/tmp/strikemq/data";
    uint64_t segment_size = 1073741824;
    uint64_t retention_bytes = static_cast<uint64_t>(-1);
    uint64_t retention_ms = 604800000;
    uint32_t message_pool_blocks = 1048576;
    uint32_t message_pool_block_size = 4096;
    bool adaptive_batching = true;
    double batch_threshold_fill_ratio = 0.3;
    uint32_t max_batch_size = 256;
    uint16_t http_port = 8080;
    int32_t broker_id = 0;
    int16_t replication_factor = 1;
    bool busy_poll = false;
    uint32_t io_uring_sq_entries = 256;
    uint32_t io_uring_buf_count = 512;
    std::vector<std::string> bootstrap_brokers;
};

/// Cross-platform high-resolution timestamp
#if defined(__x86_64__) || defined(_M_X64)
inline uint64_t rdtsc() noexcept {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
#elif defined(__aarch64__) || defined(_M_ARM64)
// Apple Silicon: use cntvct_el0 (virtual counter)
inline uint64_t rdtsc() noexcept {
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
#else
#error "Unsupported architecture"
#endif

inline void prefetch_read(const void* ptr) noexcept { __builtin_prefetch(ptr, 0, 3); }
inline void prefetch_write(void* ptr) noexcept { __builtin_prefetch(ptr, 1, 3); }

} // namespace strike
