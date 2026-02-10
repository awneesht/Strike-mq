#pragma once
#include "core/types.h"
#include "utils/endian.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace blaze {
namespace protocol {

enum class ApiKey : int16_t {
    Produce = 0, Fetch = 1, ListOffsets = 2, Metadata = 3,
    OffsetCommit = 8, OffsetFetch = 9, FindCoordinator = 10,
    JoinGroup = 11, Heartbeat = 12, LeaveGroup = 13, SyncGroup = 14, ApiVersions = 18,
};

enum class ErrorCode : int16_t {
    None = 0, Unknown = -1, OffsetOutOfRange = 1,
    UnknownTopicOrPartition = 3, LeaderNotAvailable = 5,
    NotLeaderForPartition = 6, RequestTimedOut = 7,
    NotCoordinator = 16, IllegalGeneration = 22,
    UnknownMemberId = 25, RebalanceInProgress = 27,
};

struct RequestHeader {
    int32_t message_size;
    ApiKey api_key;
    int16_t api_version;
    int32_t correlation_id;
    std::string_view client_id;
};

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t length) : data_(data), length_(length), pos_(0) {}
    [[nodiscard]] bool has_remaining(size_t n) const { return pos_ + n <= length_; }
    [[nodiscard]] size_t remaining() const { return length_ - pos_; }

    int8_t read_int8() { int8_t v; std::memcpy(&v, data_ + pos_, 1); pos_ += 1; return v; }
    int16_t read_int16() { uint16_t v; std::memcpy(&v, data_ + pos_, 2); pos_ += 2; return static_cast<int16_t>(blaze::bswap16(v)); }
    int32_t read_int32() { uint32_t v; std::memcpy(&v, data_ + pos_, 4); pos_ += 4; return static_cast<int32_t>(blaze::bswap32(v)); }
    int64_t read_int64() { uint64_t v; std::memcpy(&v, data_ + pos_, 8); pos_ += 8; return static_cast<int64_t>(blaze::bswap64(v)); }

    std::string_view read_string() {
        int16_t len = read_int16();
        if (len < 0) return {};
        std::string_view r(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len; return r;
    }

    std::span<const uint8_t> read_bytes() {
        int32_t len = read_int32();
        if (len < 0) return {};
        auto r = std::span<const uint8_t>(data_ + pos_, len);
        pos_ += len; return r;
    }

    int32_t read_signed_varint() {
        uint32_t n = 0; int shift = 0;
        while (true) {
            uint8_t b = data_[pos_++];
            n |= static_cast<uint32_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        return static_cast<int32_t>((n >> 1) ^ -(n & 1));
    }

    void skip(size_t n) { pos_ += n; }
    const uint8_t* current() const { return data_ + pos_; }
private:
    const uint8_t* data_; size_t length_; size_t pos_;
};

class BinaryWriter {
public:
    BinaryWriter(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity), pos_(0) {}

    void write_int8(int8_t v) { data_[pos_++] = static_cast<uint8_t>(v); }
    void write_int16(int16_t v) { uint16_t be = blaze::bswap16(static_cast<uint16_t>(v)); std::memcpy(data_ + pos_, &be, 2); pos_ += 2; }
    void write_int32(int32_t v) { uint32_t be = blaze::bswap32(static_cast<uint32_t>(v)); std::memcpy(data_ + pos_, &be, 4); pos_ += 4; }
    void write_int64(int64_t v) { uint64_t be = blaze::bswap64(static_cast<uint64_t>(v)); std::memcpy(data_ + pos_, &be, 8); pos_ += 8; }

    void write_string(std::string_view s) {
        write_int16(static_cast<int16_t>(s.size()));
        std::memcpy(data_ + pos_, s.data(), s.size()); pos_ += s.size();
    }
    void write_nullable_string(std::string_view s) {
        if (s.empty()) write_int16(-1); else write_string(s);
    }

    size_t write_size_placeholder() { size_t p = pos_; pos_ += 4; return p; }
    void patch_size(size_t ph) {
        int32_t sz = static_cast<int32_t>(pos_ - ph - 4);
        uint32_t be = blaze::bswap32(static_cast<uint32_t>(sz));
        std::memcpy(data_ + ph, &be, 4);
    }

    void write_raw(const uint8_t* src, size_t len) {
        std::memcpy(data_ + pos_, src, len); pos_ += len;
    }
    [[nodiscard]] size_t position() const { return pos_; }
    [[nodiscard]] uint8_t* data() { return data_; }
private:
    uint8_t* data_; size_t capacity_; size_t pos_;
};

class KafkaDecoder {
public:
    static RequestHeader decode_header(BinaryReader& r);
    static ProduceRequest decode_produce(BinaryReader& r, const RequestHeader& h);
    static FetchRequest decode_fetch(BinaryReader& r, const RequestHeader& h);
    static ListOffsetsRequest decode_list_offsets(BinaryReader& r, const RequestHeader& h);
    static RecordBatch decode_record_batch(BinaryReader& r);
    static FindCoordinatorRequest decode_find_coordinator(BinaryReader& r, const RequestHeader& h);
    static JoinGroupRequest decode_join_group(BinaryReader& r, const RequestHeader& h);
    static SyncGroupRequest decode_sync_group(BinaryReader& r, const RequestHeader& h);
    static HeartbeatRequest decode_heartbeat(BinaryReader& r, const RequestHeader& h);
    static LeaveGroupRequest decode_leave_group(BinaryReader& r, const RequestHeader& h);
    static OffsetCommitRequest decode_offset_commit(BinaryReader& r, const RequestHeader& h);
    static OffsetFetchRequest decode_offset_fetch(BinaryReader& r, const RequestHeader& h);
};

class KafkaEncoder {
public:
    static size_t encode_produce_response(uint8_t* buf, size_t cap, int32_t corr_id,
        const std::vector<ProducePartitionResponse>& parts, const std::string& topic);
    static size_t encode_api_versions_response(uint8_t* buf, size_t cap, int32_t corr_id,
        int16_t api_version);
    static size_t encode_fetch_response(uint8_t* buf, size_t cap, int32_t corr_id,
        const std::vector<FetchPartitionResponse>& parts, int16_t api_version);
    static size_t encode_list_offsets_response(uint8_t* buf, size_t cap, int32_t corr_id,
        const std::vector<ListOffsetsPartitionResponse>& parts, int16_t api_version);
    static size_t encode_metadata_response(uint8_t* buf, size_t cap, int32_t corr_id,
        int32_t broker_id, const std::string& host, uint16_t port,
        const std::vector<std::string>& topics, int32_t num_partitions);
    static size_t encode_find_coordinator_response(uint8_t* buf, size_t cap, int32_t corr_id,
        int16_t api_version, int16_t error_code, int32_t node_id,
        const std::string& host, int32_t port);
    static size_t encode_join_group_response(uint8_t* buf, size_t cap, int32_t corr_id,
        int16_t api_version, const JoinGroupResponse& resp);
    static size_t encode_sync_group_response(uint8_t* buf, size_t cap, int32_t corr_id,
        int16_t api_version, const SyncGroupResponse& resp);
    static size_t encode_heartbeat_response(uint8_t* buf, size_t cap, int32_t corr_id,
        int16_t api_version, int16_t error_code);
    static size_t encode_leave_group_response(uint8_t* buf, size_t cap, int32_t corr_id,
        int16_t api_version, int16_t error_code);
    static size_t encode_offset_commit_response(uint8_t* buf, size_t cap, int32_t corr_id,
        int16_t api_version, const std::vector<OffsetCommitPartitionResponse>& parts);
    static size_t encode_offset_fetch_response(uint8_t* buf, size_t cap, int32_t corr_id,
        int16_t api_version, const std::vector<OffsetFetchPartitionResponse>& parts,
        int16_t group_error_code);
};

struct MetadataInfo {
    int32_t broker_id = 0;
    std::string host = "localhost";
    uint16_t port = 9092;
    std::vector<std::string> topics;
    int32_t num_partitions = 1;
};

class RequestRouter {
public:
    using ProduceHandler = std::function<std::vector<ProducePartitionResponse>(const ProduceRequest&)>;
    using FetchHandler = std::function<std::vector<FetchPartitionResponse>(const FetchRequest&)>;
    using MetadataHandler = std::function<MetadataInfo(const std::vector<std::string>& requested_topics)>;
    using ListOffsetsHandler = std::function<std::vector<ListOffsetsPartitionResponse>(const ListOffsetsRequest&)>;
    using FindCoordinatorHandler = std::function<void(const FindCoordinatorRequest&, int16_t& error_code, int32_t& node_id, std::string& host, int32_t& port)>;
    using JoinGroupHandler = std::function<JoinGroupResponse(const JoinGroupRequest&)>;
    using SyncGroupHandler = std::function<SyncGroupResponse(const SyncGroupRequest&)>;
    using HeartbeatHandler = std::function<int16_t(const HeartbeatRequest&)>;
    using LeaveGroupHandler = std::function<int16_t(const LeaveGroupRequest&)>;
    using OffsetCommitHandler = std::function<std::vector<OffsetCommitPartitionResponse>(const OffsetCommitRequest&)>;
    using OffsetFetchHandler = std::function<std::vector<OffsetFetchPartitionResponse>(const OffsetFetchRequest&)>;
    void set_produce_handler(ProduceHandler h) { produce_handler_ = std::move(h); }
    void set_fetch_handler(FetchHandler h) { fetch_handler_ = std::move(h); }
    void set_metadata_handler(MetadataHandler h) { metadata_handler_ = std::move(h); }
    void set_list_offsets_handler(ListOffsetsHandler h) { list_offsets_handler_ = std::move(h); }
    void set_find_coordinator_handler(FindCoordinatorHandler h) { find_coordinator_handler_ = std::move(h); }
    void set_join_group_handler(JoinGroupHandler h) { join_group_handler_ = std::move(h); }
    void set_sync_group_handler(SyncGroupHandler h) { sync_group_handler_ = std::move(h); }
    void set_heartbeat_handler(HeartbeatHandler h) { heartbeat_handler_ = std::move(h); }
    void set_leave_group_handler(LeaveGroupHandler h) { leave_group_handler_ = std::move(h); }
    void set_offset_commit_handler(OffsetCommitHandler h) { offset_commit_handler_ = std::move(h); }
    void set_offset_fetch_handler(OffsetFetchHandler h) { offset_fetch_handler_ = std::move(h); }
    size_t route_request(const uint8_t* req, uint32_t len, uint8_t* resp, size_t cap);
private:
    ProduceHandler produce_handler_;
    FetchHandler fetch_handler_;
    MetadataHandler metadata_handler_;
    ListOffsetsHandler list_offsets_handler_;
    FindCoordinatorHandler find_coordinator_handler_;
    JoinGroupHandler join_group_handler_;
    SyncGroupHandler sync_group_handler_;
    HeartbeatHandler heartbeat_handler_;
    LeaveGroupHandler leave_group_handler_;
    OffsetCommitHandler offset_commit_handler_;
    OffsetFetchHandler offset_fetch_handler_;
};

} // namespace protocol
} // namespace blaze
