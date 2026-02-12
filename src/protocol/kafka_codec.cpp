#include "protocol/kafka_codec.h"

namespace strike { namespace protocol {

RequestHeader KafkaDecoder::decode_header(BinaryReader& r) {
    RequestHeader h;
    h.api_key = static_cast<ApiKey>(r.read_int16());
    h.api_version = r.read_int16();
    h.correlation_id = r.read_int32();
    h.client_id = r.read_string();
    return h;
}

ProduceRequest KafkaDecoder::decode_produce(BinaryReader& r, const RequestHeader& h) {
    ProduceRequest req;
    req.correlation_id = h.correlation_id;
    req.client_id = std::string(h.client_id);
    if (h.api_version >= 3) r.read_string();
    req.acks = r.read_int16();
    req.timeout_ms = r.read_int32();
    int32_t nt = r.read_int32();
    for (int32_t t = 0; t < nt; ++t) {
        auto topic = r.read_string();
        int32_t np = r.read_int32();
        for (int32_t p = 0; p < np; ++p) {
            int32_t pid = r.read_int32();
            auto data = r.read_bytes();
            if (!data.empty()) {
                BinaryReader br(data.data(), data.size());
                RecordBatch batch = decode_record_batch(br);
                batch.topic_partition = {std::string(topic), pid};
                req.batches.push_back(std::move(batch));
            }
        }
    }
    return req;
}

RecordBatch KafkaDecoder::decode_record_batch(BinaryReader& r) {
    RecordBatch b;
    b.base_offset = r.read_int64();
    r.read_int32(); // batch length
    b.partition_leader_epoch = r.read_int32();
    b.magic = r.read_int8();
    if (b.magic != 2) return b;
    b.crc = r.read_int32(); b.attributes = r.read_int16();
    int32_t last_off_d = r.read_int32();
    b.first_timestamp = r.read_int64(); b.max_timestamp = r.read_int64();
    b.producer_id = r.read_int64(); b.producer_epoch = r.read_int16();
    b.base_sequence = r.read_int32();
    int32_t nr = r.read_int32();
    for (int32_t i = 0; i < nr; ++i) {
        Record rec;
        const uint8_t* rec_start = r.current();
        r.read_signed_varint(); r.read_int8(); // length, attributes
        int32_t td = r.read_signed_varint(); r.read_signed_varint(); // timestampDelta, offsetDelta
        int32_t kl = r.read_signed_varint(); if (kl >= 0) { rec.key_length = kl; r.skip(kl); }
        int32_t vl = r.read_signed_varint(); if (vl >= 0) { rec.value_offset = rec.key_length; rec.value_length = vl; r.skip(vl); }
        int32_t nh = r.read_signed_varint();
        for (int32_t h = 0; h < nh; ++h) { int32_t hk=r.read_signed_varint(); r.skip(hk); int32_t hv=r.read_signed_varint(); if(hv>=0) r.skip(hv); }
        rec.timestamp = b.first_timestamp + td;
        rec.data = const_cast<uint8_t*>(rec_start);
        rec.total_size = static_cast<uint32_t>(r.current() - rec_start);
        b.records.push_back(std::move(rec));
    }
    b.last_offset = b.base_offset + last_off_d;
    return b;
}

FetchRequest KafkaDecoder::decode_fetch(BinaryReader& r, const RequestHeader& h) {
    FetchRequest req;
    req.correlation_id = h.correlation_id;
    r.read_int32(); // replica_id
    req.max_wait_ms = r.read_int32();
    req.min_bytes = r.read_int32();
    if (h.api_version >= 3) req.max_bytes = r.read_int32();
    if (h.api_version >= 4) r.read_int8();
    int32_t nt = r.read_int32();
    for (int32_t t = 0; t < nt; ++t) {
        std::string topic_str(r.read_string()); // hoist: one copy per topic
        int32_t np = r.read_int32();
        for (int32_t p = 0; p < np; ++p) {
            FetchRequest::PartitionFetch pf;
            pf.tp.topic = topic_str;
            pf.tp.partition = r.read_int32();
            if (h.api_version >= 9) r.read_int32();
            pf.fetch_offset = r.read_int64();
            if (h.api_version >= 5) r.read_int64();
            pf.partition_max_bytes = r.read_int32();
            req.partitions.push_back(std::move(pf));
        }
    }
    return req;
}

// ---- Consumer Group Decoders ----

FindCoordinatorRequest KafkaDecoder::decode_find_coordinator(BinaryReader& r, const RequestHeader& h) {
    FindCoordinatorRequest req;
    req.correlation_id = h.correlation_id;
    req.api_version = h.api_version;
    req.group_id = std::string(r.read_string());
    if (h.api_version >= 1) req.coordinator_type = r.read_int8();
    return req;
}

JoinGroupRequest KafkaDecoder::decode_join_group(BinaryReader& r, const RequestHeader& h) {
    JoinGroupRequest req;
    req.correlation_id = h.correlation_id;
    req.api_version = h.api_version;
    req.client_id = std::string(h.client_id);
    req.group_id = std::string(r.read_string());
    req.session_timeout_ms = r.read_int32();
    if (h.api_version >= 1) req.rebalance_timeout_ms = r.read_int32();
    req.member_id = std::string(r.read_string());
    req.protocol_type = std::string(r.read_string());
    int32_t np = r.read_int32();
    for (int32_t i = 0; i < np; ++i) {
        JoinGroupProtocol proto;
        proto.name = std::string(r.read_string());
        auto bytes = r.read_bytes();
        proto.metadata.assign(bytes.begin(), bytes.end());
        req.protocols.push_back(std::move(proto));
    }
    return req;
}

SyncGroupRequest KafkaDecoder::decode_sync_group(BinaryReader& r, const RequestHeader& h) {
    SyncGroupRequest req;
    req.correlation_id = h.correlation_id;
    req.api_version = h.api_version;
    req.group_id = std::string(r.read_string());
    req.generation_id = r.read_int32();
    req.member_id = std::string(r.read_string());
    int32_t na = r.read_int32();
    for (int32_t i = 0; i < na; ++i) {
        SyncGroupAssignment a;
        a.member_id = std::string(r.read_string());
        auto bytes = r.read_bytes();
        a.assignment.assign(bytes.begin(), bytes.end());
        req.assignments.push_back(std::move(a));
    }
    return req;
}

HeartbeatRequest KafkaDecoder::decode_heartbeat(BinaryReader& r, const RequestHeader& h) {
    HeartbeatRequest req;
    req.correlation_id = h.correlation_id;
    req.api_version = h.api_version;
    req.group_id = std::string(r.read_string());
    req.generation_id = r.read_int32();
    req.member_id = std::string(r.read_string());
    return req;
}

LeaveGroupRequest KafkaDecoder::decode_leave_group(BinaryReader& r, const RequestHeader& h) {
    LeaveGroupRequest req;
    req.correlation_id = h.correlation_id;
    req.api_version = h.api_version;
    req.group_id = std::string(r.read_string());
    req.member_id = std::string(r.read_string());
    return req;
}

OffsetCommitRequest KafkaDecoder::decode_offset_commit(BinaryReader& r, const RequestHeader& h) {
    OffsetCommitRequest req;
    req.correlation_id = h.correlation_id;
    req.api_version = h.api_version;
    req.group_id = std::string(r.read_string());
    if (h.api_version >= 1) {
        req.generation_id = r.read_int32();
        req.member_id = std::string(r.read_string());
    }
    if (h.api_version >= 2) req.retention_time_ms = r.read_int64();
    int32_t nt = r.read_int32();
    for (int32_t t = 0; t < nt; ++t) {
        std::string topic_str(r.read_string()); // hoist: one copy per topic
        int32_t np = r.read_int32();
        for (int32_t p = 0; p < np; ++p) {
            OffsetCommitPartitionRequest pr;
            pr.tp.topic = topic_str;
            pr.tp.partition = r.read_int32();
            pr.offset = r.read_int64();
            if (h.api_version == 1) pr.timestamp = r.read_int64();
            pr.metadata = std::string(r.read_string());
            req.partitions.push_back(std::move(pr));
        }
    }
    return req;
}

OffsetFetchRequest KafkaDecoder::decode_offset_fetch(BinaryReader& r, const RequestHeader& h) {
    OffsetFetchRequest req;
    req.correlation_id = h.correlation_id;
    req.api_version = h.api_version;
    req.group_id = std::string(r.read_string());
    int32_t nt = r.read_int32();
    for (int32_t t = 0; t < nt; ++t) {
        std::string topic_str(r.read_string()); // hoist: one copy per topic
        int32_t np = r.read_int32();
        for (int32_t p = 0; p < np; ++p) {
            OffsetFetchPartitionRequest pr;
            pr.tp.topic = topic_str;
            pr.tp.partition = r.read_int32();
            req.partitions.push_back(std::move(pr));
        }
    }
    return req;
}

// ---- Existing Encoders ----

size_t KafkaEncoder::encode_produce_response(uint8_t* buf, size_t cap, int32_t cid,
    const std::vector<ProducePartitionResponse>& parts, const std::string& topic) {
    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();
    w.write_int32(cid);
    w.write_int32(1); w.write_string(topic);
    w.write_int32(static_cast<int32_t>(parts.size()));
    for (const auto& p : parts) {
        w.write_int32(p.partition); w.write_int16(p.error_code);
        w.write_int64(p.base_offset); w.write_int64(p.log_append_time);
        w.write_int64(p.log_start_offset);
    }
    w.write_int32(0);
    w.patch_size(sp);
    return w.position();
}

ListOffsetsRequest KafkaDecoder::decode_list_offsets(BinaryReader& r, const RequestHeader& h) {
    ListOffsetsRequest req;
    req.correlation_id = h.correlation_id;
    req.api_version = h.api_version;
    r.read_int32(); // replica_id
    if (h.api_version >= 2) r.read_int8(); // isolation_level
    int32_t nt = r.read_int32();
    for (int32_t t = 0; t < nt; ++t) {
        std::string topic_str(r.read_string()); // hoist: one copy per topic
        int32_t np = r.read_int32();
        for (int32_t p = 0; p < np; ++p) {
            ListOffsetsRequest::PartitionRequest pr;
            pr.tp.topic = topic_str;
            pr.tp.partition = r.read_int32();
            pr.timestamp = r.read_int64();
            if (h.api_version == 0) r.read_int32(); // max_num_offsets (v0 only)
            req.partitions.push_back(std::move(pr));
        }
    }
    return req;
}

size_t KafkaEncoder::encode_list_offsets_response(uint8_t* buf, size_t cap, int32_t cid,
    const std::vector<ListOffsetsPartitionResponse>& parts, int16_t api_version) {
    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();
    w.write_int32(cid);

    // v2+ adds throttle_time_ms
    if (api_version >= 2) w.write_int32(0);

    // Group by topic
    std::vector<std::string> topics;
    for (const auto& p : parts) {
        bool found = false;
        for (const auto& t : topics) { if (t == p.tp.topic) { found = true; break; } }
        if (!found) topics.push_back(p.tp.topic);
    }

    w.write_int32(static_cast<int32_t>(topics.size()));
    for (const auto& topic : topics) {
        w.write_string(topic);
        int32_t np = 0;
        for (const auto& p : parts) { if (p.tp.topic == topic) ++np; }
        w.write_int32(np);
        for (const auto& p : parts) {
            if (p.tp.topic != topic) continue;
            w.write_int32(p.tp.partition);
            w.write_int16(p.error_code);
            if (api_version == 0) {
                // v0: array of offsets
                w.write_int32(1); // one offset
                w.write_int64(p.offset);
            } else {
                // v1+: timestamp + single offset
                w.write_int64(p.timestamp);
                w.write_int64(p.offset);
            }
        }
    }

    w.patch_size(sp);
    return w.position();
}

size_t KafkaEncoder::encode_fetch_response(uint8_t* buf, size_t cap, int32_t cid,
    const std::vector<FetchPartitionResponse>& parts, int16_t api_version) {
    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();
    w.write_int32(cid);

    // v1+ adds throttle_time_ms
    if (api_version >= 1) w.write_int32(0); // throttle_time_ms

    // Group partitions by topic
    std::vector<std::string> topics;
    for (const auto& p : parts) {
        bool found = false;
        for (const auto& t : topics) { if (t == p.tp.topic) { found = true; break; } }
        if (!found) topics.push_back(p.tp.topic);
    }

    w.write_int32(static_cast<int32_t>(topics.size()));
    for (const auto& topic : topics) {
        w.write_string(topic);
        int32_t np = 0;
        for (const auto& p : parts) { if (p.tp.topic == topic) ++np; }
        w.write_int32(np);
        for (const auto& p : parts) {
            if (p.tp.topic != topic) continue;
            w.write_int32(p.tp.partition);
            w.write_int16(p.error_code);
            w.write_int64(p.high_watermark);
            // v4+ adds last_stable_offset and aborted_transactions
            if (api_version >= 4) {
                w.write_int64(p.high_watermark); // last_stable_offset = high_watermark
                w.write_int32(-1); // aborted_transactions: null array (-1)
            }
            // Record batches as raw bytes
            w.write_int32(static_cast<int32_t>(p.record_data_size));
            if (p.record_data && p.record_data_size > 0) {
                w.write_raw(p.record_data, p.record_data_size);
            }
        }
    }

    w.patch_size(sp);
    return w.position();
}

// Helper: write unsigned varint (Kafka compact encoding)
static void write_uvarint(BinaryWriter& w, uint32_t val) {
    while (val > 0x7F) {
        w.write_int8(static_cast<int8_t>((val & 0x7F) | 0x80));
        val >>= 7;
    }
    w.write_int8(static_cast<int8_t>(val));
}

size_t KafkaEncoder::encode_api_versions_response(uint8_t* buf, size_t cap, int32_t cid,
    int16_t api_version) {
    // Advertise a standard Kafka broker API set so librdkafka allocates correctly
    struct { int16_t k, mn, mx; } apis[] = {
        {0, 0, 5},   // Produce
        {1, 0, 4},   // Fetch (v0-v4 — v4 needed for MsgVer2/record batches)
        {2, 0, 2},   // ListOffsets
        {3, 0, 0},   // Metadata (v0 only)
        {8, 0, 3},   // OffsetCommit
        {9, 0, 3},   // OffsetFetch
        {10, 0, 2},  // FindCoordinator
        {11, 0, 3},  // JoinGroup
        {12, 0, 2},  // Heartbeat
        {13, 0, 1},  // LeaveGroup
        {14, 0, 2},  // SyncGroup
        {18, 0, 3},  // ApiVersions
        {19, 0, 3},  // CreateTopics
    };
    constexpr int num_apis = 13;

    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();

    if (api_version >= 3) {
        // Flexible versions (v3): compact arrays + tagged fields
        // NOTE: ApiVersions is special — no header tagged_fields for backwards compat
        w.write_int32(cid);
        w.write_int16(0); // error_code
        write_uvarint(w, num_apis + 1); // compact array length (N+1)
        for (auto& a : apis) {
            w.write_int16(a.k);
            w.write_int16(a.mn);
            w.write_int16(a.mx);
            w.write_int8(0); // per-entry tagged_fields (empty)
        }
        w.write_int32(0); // throttle_time_ms
        w.write_int8(0);  // body tagged_fields (empty)
    } else {
        // Non-flexible (v0-v2)
        w.write_int32(cid);
        w.write_int16(0); // error_code
        w.write_int32(num_apis);
        for (auto& a : apis) {
            w.write_int16(a.k);
            w.write_int16(a.mn);
            w.write_int16(a.mx);
        }
        if (api_version >= 1) {
            w.write_int32(0); // throttle_time_ms (v1+)
        }
    }

    w.patch_size(sp);
    return w.position();
}

size_t KafkaEncoder::encode_metadata_response(uint8_t* buf, size_t cap, int32_t cid,
    int32_t bid, const std::string& host, uint16_t port,
    const std::vector<std::string>& topics, int32_t np) {
    // Metadata Response v0 wire format
    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();
    w.write_int32(cid);
    // Brokers array
    w.write_int32(1); // num brokers
    w.write_int32(bid);
    w.write_string(host);
    w.write_int32(static_cast<int32_t>(port));
    // Topics array
    w.write_int32(static_cast<int32_t>(topics.size()));
    for (const auto& t : topics) {
        w.write_int16(0); // error_code
        w.write_string(t);
        w.write_int32(np); // num partitions
        for (int32_t p = 0; p < np; ++p) {
            w.write_int16(0);  // error_code
            w.write_int32(p);  // partition_id
            w.write_int32(bid); // leader
            w.write_int32(1); w.write_int32(bid); // replicas array (1 element)
            w.write_int32(1); w.write_int32(bid); // isr array (1 element)
        }
    }
    w.patch_size(sp);
    return w.position();
}

// ---- Consumer Group Encoders ----

size_t KafkaEncoder::encode_find_coordinator_response(uint8_t* buf, size_t cap, int32_t cid,
    int16_t api_version, int16_t error_code, int32_t node_id,
    const std::string& host, int32_t port) {
    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();
    w.write_int32(cid);
    if (api_version >= 1) w.write_int32(0); // throttle_time_ms
    w.write_int16(error_code);
    if (api_version >= 1) w.write_string(""); // error_message
    w.write_int32(node_id);
    w.write_string(host);
    w.write_int32(port);
    w.patch_size(sp);
    return w.position();
}

size_t KafkaEncoder::encode_join_group_response(uint8_t* buf, size_t cap, int32_t cid,
    int16_t api_version, const JoinGroupResponse& resp) {
    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();
    w.write_int32(cid);
    if (api_version >= 2) w.write_int32(0); // throttle_time_ms
    w.write_int16(resp.error_code);
    w.write_int32(resp.generation_id);
    w.write_string(resp.protocol_name);
    w.write_string(resp.leader_id);
    w.write_string(resp.member_id);
    w.write_int32(static_cast<int32_t>(resp.members.size()));
    for (const auto& m : resp.members) {
        w.write_string(m.member_id);
        // Member metadata is BYTES (int32 length-prefix)
        w.write_int32(static_cast<int32_t>(m.metadata.size()));
        if (!m.metadata.empty()) {
            w.write_raw(m.metadata.data(), m.metadata.size());
        }
    }
    w.patch_size(sp);
    return w.position();
}

size_t KafkaEncoder::encode_sync_group_response(uint8_t* buf, size_t cap, int32_t cid,
    int16_t api_version, const SyncGroupResponse& resp) {
    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();
    w.write_int32(cid);
    if (api_version >= 1) w.write_int32(0); // throttle_time_ms
    w.write_int16(resp.error_code);
    // Assignment is BYTES (int32 length-prefix)
    w.write_int32(static_cast<int32_t>(resp.member_assignment.size()));
    if (!resp.member_assignment.empty()) {
        w.write_raw(resp.member_assignment.data(), resp.member_assignment.size());
    }
    w.patch_size(sp);
    return w.position();
}

size_t KafkaEncoder::encode_heartbeat_response(uint8_t* buf, size_t cap, int32_t cid,
    int16_t api_version, int16_t error_code) {
    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();
    w.write_int32(cid);
    if (api_version >= 1) w.write_int32(0); // throttle_time_ms
    w.write_int16(error_code);
    w.patch_size(sp);
    return w.position();
}

size_t KafkaEncoder::encode_leave_group_response(uint8_t* buf, size_t cap, int32_t cid,
    int16_t api_version, int16_t error_code) {
    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();
    w.write_int32(cid);
    if (api_version >= 1) w.write_int32(0); // throttle_time_ms
    w.write_int16(error_code);
    w.patch_size(sp);
    return w.position();
}

size_t KafkaEncoder::encode_offset_commit_response(uint8_t* buf, size_t cap, int32_t cid,
    int16_t api_version, const std::vector<OffsetCommitPartitionResponse>& parts) {
    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();
    w.write_int32(cid);
    if (api_version >= 3) w.write_int32(0); // throttle_time_ms

    // Group by topic
    std::vector<std::string> topics;
    for (const auto& p : parts) {
        bool found = false;
        for (const auto& t : topics) { if (t == p.tp.topic) { found = true; break; } }
        if (!found) topics.push_back(p.tp.topic);
    }

    w.write_int32(static_cast<int32_t>(topics.size()));
    for (const auto& topic : topics) {
        w.write_string(topic);
        int32_t np = 0;
        for (const auto& p : parts) { if (p.tp.topic == topic) ++np; }
        w.write_int32(np);
        for (const auto& p : parts) {
            if (p.tp.topic != topic) continue;
            w.write_int32(p.tp.partition);
            w.write_int16(p.error_code);
        }
    }

    w.patch_size(sp);
    return w.position();
}

size_t KafkaEncoder::encode_offset_fetch_response(uint8_t* buf, size_t cap, int32_t cid,
    int16_t api_version, const std::vector<OffsetFetchPartitionResponse>& parts,
    int16_t group_error_code) {
    BinaryWriter w(buf, cap);
    size_t sp = w.write_size_placeholder();
    w.write_int32(cid);
    if (api_version >= 3) w.write_int32(0); // throttle_time_ms

    // Group by topic
    std::vector<std::string> topics;
    for (const auto& p : parts) {
        bool found = false;
        for (const auto& t : topics) { if (t == p.tp.topic) { found = true; break; } }
        if (!found) topics.push_back(p.tp.topic);
    }

    w.write_int32(static_cast<int32_t>(topics.size()));
    for (const auto& topic : topics) {
        w.write_string(topic);
        int32_t np = 0;
        for (const auto& p : parts) { if (p.tp.topic == topic) ++np; }
        w.write_int32(np);
        for (const auto& p : parts) {
            if (p.tp.topic != topic) continue;
            w.write_int32(p.tp.partition);
            w.write_int64(p.offset);
            w.write_string(p.metadata);
            w.write_int16(p.error_code);
        }
    }

    // v2+ adds group-level error_code
    if (api_version >= 2) w.write_int16(group_error_code);

    w.patch_size(sp);
    return w.position();
}

size_t RequestRouter::route_request(const uint8_t* req, uint32_t len, uint8_t* resp, size_t cap) {
    // Peek at api_key and api_version (always at offset 0 and 2)
    BinaryReader peek(req, len);
    auto api_key = static_cast<ApiKey>(peek.read_int16());
    int16_t api_version = peek.read_int16();
    int32_t correlation_id = peek.read_int32();

    // ApiVersions needs special handling: v3+ uses flexible headers which
    // our normal decode_header can't parse (compact strings, tagged fields).
    // Since ApiVersions doesn't need any body decoding, handle it inline.
    if (api_key == ApiKey::ApiVersions) {
        return KafkaEncoder::encode_api_versions_response(resp, cap, correlation_id, api_version);
    }

    // All other APIs: use standard header decoding
    BinaryReader r(req, len);
    auto h = KafkaDecoder::decode_header(r);
    switch (h.api_key) {
    case ApiKey::Produce: {
        auto request = KafkaDecoder::decode_produce(r, h);
        if (produce_handler_) {
            auto responses = produce_handler_(request);
            std::string topic = request.batches.empty() ? "" : request.batches[0].topic_partition.topic;
            return KafkaEncoder::encode_produce_response(resp, cap, h.correlation_id, responses, topic);
        }
        break;
    }
    case ApiKey::ListOffsets: {
        auto request = KafkaDecoder::decode_list_offsets(r, h);
        if (list_offsets_handler_) {
            auto responses = list_offsets_handler_(request);
            return KafkaEncoder::encode_list_offsets_response(resp, cap, h.correlation_id, responses, h.api_version);
        }
        break;
    }
    case ApiKey::Fetch: {
        auto request = KafkaDecoder::decode_fetch(r, h);
        if (fetch_handler_) {
            auto responses = fetch_handler_(request);
            return KafkaEncoder::encode_fetch_response(resp, cap, h.correlation_id, responses, h.api_version);
        }
        break;
    }
    case ApiKey::Metadata: {
        if (metadata_handler_) {
            // Parse requested topics from Metadata v0 request
            std::vector<std::string> requested;
            int32_t num_topics = r.read_int32();
            if (num_topics > 0) {
                for (int32_t t = 0; t < num_topics; ++t)
                    requested.emplace_back(r.read_string());
            }
            // num_topics == -1 means "all topics", 0 means "none"
            auto info = metadata_handler_(requested);
            return KafkaEncoder::encode_metadata_response(resp, cap, h.correlation_id,
                info.broker_id, info.host, info.port, info.topics, info.num_partitions);
        }
        break;
    }
    case ApiKey::FindCoordinator: {
        auto request = KafkaDecoder::decode_find_coordinator(r, h);
        if (find_coordinator_handler_) {
            int16_t error_code = 0;
            int32_t node_id = 0;
            std::string host;
            int32_t port = 0;
            find_coordinator_handler_(request, error_code, node_id, host, port);
            return KafkaEncoder::encode_find_coordinator_response(resp, cap, h.correlation_id,
                h.api_version, error_code, node_id, host, port);
        }
        break;
    }
    case ApiKey::JoinGroup: {
        auto request = KafkaDecoder::decode_join_group(r, h);
        if (join_group_handler_) {
            auto response = join_group_handler_(request);
            return KafkaEncoder::encode_join_group_response(resp, cap, h.correlation_id,
                h.api_version, response);
        }
        break;
    }
    case ApiKey::SyncGroup: {
        auto request = KafkaDecoder::decode_sync_group(r, h);
        if (sync_group_handler_) {
            auto response = sync_group_handler_(request);
            return KafkaEncoder::encode_sync_group_response(resp, cap, h.correlation_id,
                h.api_version, response);
        }
        break;
    }
    case ApiKey::Heartbeat: {
        auto request = KafkaDecoder::decode_heartbeat(r, h);
        if (heartbeat_handler_) {
            int16_t error_code = heartbeat_handler_(request);
            return KafkaEncoder::encode_heartbeat_response(resp, cap, h.correlation_id,
                h.api_version, error_code);
        }
        break;
    }
    case ApiKey::LeaveGroup: {
        auto request = KafkaDecoder::decode_leave_group(r, h);
        if (leave_group_handler_) {
            int16_t error_code = leave_group_handler_(request);
            return KafkaEncoder::encode_leave_group_response(resp, cap, h.correlation_id,
                h.api_version, error_code);
        }
        break;
    }
    case ApiKey::OffsetCommit: {
        auto request = KafkaDecoder::decode_offset_commit(r, h);
        if (offset_commit_handler_) {
            auto responses = offset_commit_handler_(request);
            return KafkaEncoder::encode_offset_commit_response(resp, cap, h.correlation_id,
                h.api_version, responses);
        }
        break;
    }
    case ApiKey::OffsetFetch: {
        auto request = KafkaDecoder::decode_offset_fetch(r, h);
        if (offset_fetch_handler_) {
            auto responses = offset_fetch_handler_(request);
            return KafkaEncoder::encode_offset_fetch_response(resp, cap, h.correlation_id,
                h.api_version, responses, 0);
        }
        break;
    }
    default: break;
    }
    return 0;
}

}} // namespace strike::protocol
