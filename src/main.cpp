#include "core/types.h"
#include "protocol/kafka_codec.h"
#include "network/tcp_server.h"
#include "storage/partition_log.h"
#include "storage/consumer_group.h"
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <unordered_map>

static blaze::network::TcpServer* g_server = nullptr;

void sig_handler(int) {
    if (g_server) g_server->stop();
}

int main(int /* argc */, char* /* argv */[]) {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    blaze::BrokerConfig config; // defaults: 0.0.0.0:9092

    std::cout << "═══════════════════════════════════════════\n"
              << "  BlazeMQ v0.1.0 — Sub-Millisecond Broker\n"
              << "═══════════════════════════════════════════\n"
#ifdef BLAZE_PLATFORM_MACOS
              << "  Platform: macOS (kqueue)\n"
#else
              << "  Platform: Linux (epoll)\n"
#endif
              << "  Port: " << config.port << "\n"
              << "═══════════════════════════════════════════\n\n"
              << std::flush;

    // Storage: topic-partition -> PartitionLog (created on first produce)
    std::mutex logs_mu;
    std::unordered_map<blaze::TopicPartition,
                       std::unique_ptr<blaze::storage::PartitionLog>,
                       blaze::TopicPartitionHash> logs;

    std::mutex topics_mu;
    std::vector<std::string> known_topics;

    auto get_or_create_log = [&](const blaze::TopicPartition& tp) -> blaze::storage::PartitionLog& {
        std::lock_guard<std::mutex> lock(logs_mu);
        auto it = logs.find(tp);
        if (it != logs.end()) return *it->second;
        auto log = std::make_unique<blaze::storage::PartitionLog>(tp, config.log_dir, config.segment_size);
        auto& ref = *log;
        logs.emplace(tp, std::move(log));
        // Track topic name
        {
            std::lock_guard<std::mutex> tlock(topics_mu);
            bool found = false;
            for (const auto& t : known_topics) {
                if (t == tp.topic) { found = true; break; }
            }
            if (!found) known_topics.push_back(tp.topic);
        }
        std::cout << "  * Created partition log: " << tp.topic << "-" << tp.partition << "\n" << std::flush;
        return ref;
    };

    // Set up request router
    blaze::protocol::RequestRouter router;

    router.set_produce_handler([&](const blaze::ProduceRequest& req) {
        std::vector<blaze::ProducePartitionResponse> responses;
        for (auto& batch : req.batches) {
            auto& log = get_or_create_log(batch.topic_partition);
            auto mutable_batch = batch; // append needs non-const
            int64_t base_offset = log.append(mutable_batch);
            blaze::ProducePartitionResponse resp;
            resp.partition = batch.topic_partition.partition;
            resp.base_offset = base_offset;
            resp.error_code = 0;
            responses.push_back(resp);
        }
        return responses;
    });

    router.set_list_offsets_handler([&](const blaze::ListOffsetsRequest& req) {
        std::vector<blaze::ListOffsetsPartitionResponse> responses;
        for (const auto& pr : req.partitions) {
            blaze::ListOffsetsPartitionResponse resp;
            resp.tp = pr.tp;
            std::lock_guard<std::mutex> lock(logs_mu);
            auto it = logs.find(pr.tp);
            if (it == logs.end()) {
                resp.error_code = 3; // UNKNOWN_TOPIC_OR_PARTITION
            } else {
                auto& log = *it->second;
                if (pr.timestamp == -2) {
                    // Earliest
                    resp.offset = log.start_offset();
                } else if (pr.timestamp == -1) {
                    // Latest
                    resp.offset = log.next_offset();
                } else {
                    // Timestamp-based: return start for now
                    resp.offset = log.start_offset();
                }
                resp.timestamp = -1;
            }
            responses.push_back(resp);
        }
        return responses;
    });

    router.set_fetch_handler([&](const blaze::FetchRequest& req) {
        std::vector<blaze::FetchPartitionResponse> responses;
        for (const auto& pf : req.partitions) {
            blaze::FetchPartitionResponse resp;
            resp.tp = pf.tp;
            std::lock_guard<std::mutex> lock(logs_mu);
            auto it = logs.find(pf.tp);
            if (it == logs.end()) {
                resp.error_code = 3; // UNKNOWN_TOPIC_OR_PARTITION
            } else {
                auto result = it->second->read(pf.fetch_offset, pf.partition_max_bytes);
                resp.high_watermark = result.high_watermark;
                resp.record_data = result.data;
                resp.record_data_size = result.size;
            }
            responses.push_back(resp);
        }
        return responses;
    });

    router.set_metadata_handler([&](const std::vector<std::string>& requested) {
        // Auto-create requested topics (Kafka auto.create.topics.enable behavior)
        for (const auto& topic : requested) {
            get_or_create_log({topic, 0});
        }
        blaze::protocol::MetadataInfo info;
        info.broker_id = config.broker_id;
        info.host = config.bind_address == "0.0.0.0" ? "127.0.0.1" : config.bind_address;
        info.port = config.port;
        {
            std::lock_guard<std::mutex> lock(topics_mu);
            if (requested.empty()) {
                info.topics = known_topics; // all topics
            } else {
                info.topics = requested; // only requested
            }
        }
        info.num_partitions = 1;
        return info;
    });

    // Consumer group state
    blaze::storage::ConsumerGroupManager group_mgr;

    router.set_find_coordinator_handler([&](const blaze::FindCoordinatorRequest&,
        int16_t& error_code, int32_t& node_id, std::string& host, int32_t& port) {
        error_code = 0;
        node_id = config.broker_id;
        host = config.bind_address == "0.0.0.0" ? "127.0.0.1" : config.bind_address;
        port = static_cast<int32_t>(config.port);
    });

    router.set_join_group_handler([&](const blaze::JoinGroupRequest& req) {
        return group_mgr.join_group(req);
    });

    router.set_sync_group_handler([&](const blaze::SyncGroupRequest& req) {
        return group_mgr.sync_group(req);
    });

    router.set_heartbeat_handler([&](const blaze::HeartbeatRequest& req) {
        return group_mgr.heartbeat(req);
    });

    router.set_leave_group_handler([&](const blaze::LeaveGroupRequest& req) {
        return group_mgr.leave_group(req);
    });

    router.set_offset_commit_handler([&](const blaze::OffsetCommitRequest& req) {
        return group_mgr.offset_commit(req);
    });

    router.set_offset_fetch_handler([&](const blaze::OffsetFetchRequest& req) {
        return group_mgr.offset_fetch(req);
    });

    // Create and start TCP server
    blaze::network::TcpServer server(router);
    g_server = &server;

    if (!server.bind_and_listen(config.bind_address, config.port)) {
        std::cerr << "Failed to start server. Exiting.\n";
        return 1;
    }

    std::cout << "Broker ready. Press Ctrl+C to stop.\n\n" << std::flush;
    server.run();

    std::cout << "\nBlazeMQ stopped.\n";
    return 0;
}
