#include "core/types.h"
#include "protocol/kafka_codec.h"
#include "network/tcp_server.h"
#include "storage/partition_log.h"
#include "storage/sharded_log_map.h"
#include "storage/consumer_group.h"
#include "http/http_server.h"
#include <algorithm>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

static strike::network::TcpServer* g_server = nullptr;
static strike::http::HttpServer* g_http_server = nullptr;

void sig_handler(int) {
    if (g_http_server) g_http_server->stop();
    if (g_server) g_server->stop();
}

int main(int /* argc */, char* /* argv */[]) {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    strike::BrokerConfig config; // defaults: 0.0.0.0:9092

    size_t num_io = config.num_io_threads == 0
        ? std::max(1u, std::thread::hardware_concurrency())
        : config.num_io_threads;

    std::cout << "═══════════════════════════════════════════\n"
              << "  StrikeMQ v0.1.4 — Sub-Millisecond Broker\n"
              << "═══════════════════════════════════════════\n"
#ifdef STRIKE_PLATFORM_MACOS
              << "  Platform: macOS (kqueue)\n"
#elif defined(STRIKE_IO_URING)
              << "  Platform: Linux (io_uring)\n"
#else
              << "  Platform: Linux (epoll)\n"
#endif
              << "  Kafka Port: " << config.port << "\n"
              << "  HTTP API: " << config.http_port << "\n"
              << "  IO Threads: " << num_io
              << (config.num_io_threads == 0 ? " (auto)" : "") << "\n"
              << "═══════════════════════════════════════════\n\n"
              << std::flush;

    // Storage: sharded topic-partition -> PartitionLog map (created on first produce)
    strike::storage::ShardedLogMap<64> sharded_logs;

    std::mutex topics_mu;
    std::vector<std::string> known_topics;

    auto get_or_create_log = [&](const strike::TopicPartition& tp) -> strike::storage::PartitionLog& {
        return sharded_logs.get_or_create(tp, config.log_dir, config.segment_size,
            [&](const strike::TopicPartition& new_tp) {
                std::lock_guard<std::mutex> tlock(topics_mu);
                bool found = false;
                for (const auto& t : known_topics) {
                    if (t == new_tp.topic) { found = true; break; }
                }
                if (!found) known_topics.push_back(new_tp.topic);
                std::cout << "  * Created partition log: " << new_tp.topic << "-" << new_tp.partition << "\n" << std::flush;
            });
    };

    // Set up request router
    strike::protocol::RequestRouter router;

    router.set_produce_handler([&](const strike::ProduceRequest& req) {
        std::vector<strike::ProducePartitionResponse> responses;
        for (auto& batch : req.batches) {
            auto& log = get_or_create_log(batch.topic_partition);
            auto mutable_batch = batch; // append needs non-const
            int64_t base_offset = log.append(mutable_batch);
            strike::ProducePartitionResponse resp;
            resp.partition = batch.topic_partition.partition;
            resp.base_offset = base_offset;
            resp.error_code = 0;
            responses.push_back(resp);
        }
        return responses;
    });

    router.set_list_offsets_handler([&](const strike::ListOffsetsRequest& req) {
        std::vector<strike::ListOffsetsPartitionResponse> responses;
        for (const auto& pr : req.partitions) {
            strike::ListOffsetsPartitionResponse resp;
            resp.tp = pr.tp;
            auto* log = sharded_logs.find(pr.tp);
            if (!log) {
                resp.error_code = 3; // UNKNOWN_TOPIC_OR_PARTITION
            } else {
                if (pr.timestamp == -2) {
                    // Earliest
                    resp.offset = log->start_offset();
                } else if (pr.timestamp == -1) {
                    // Latest
                    resp.offset = log->next_offset();
                } else {
                    // Timestamp-based: return start for now
                    resp.offset = log->start_offset();
                }
                resp.timestamp = -1;
            }
            responses.push_back(resp);
        }
        return responses;
    });

    router.set_fetch_handler([&](const strike::FetchRequest& req) {
        std::vector<strike::FetchPartitionResponse> responses;
        for (const auto& pf : req.partitions) {
            strike::FetchPartitionResponse resp;
            resp.tp = pf.tp;
            auto* log = sharded_logs.find(pf.tp);
            if (!log) {
                resp.error_code = 3; // UNKNOWN_TOPIC_OR_PARTITION
            } else {
                auto result = log->read(pf.fetch_offset, pf.partition_max_bytes);
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
        strike::protocol::MetadataInfo info;
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
    strike::storage::ConsumerGroupManager group_mgr;

    router.set_find_coordinator_handler([&](const strike::FindCoordinatorRequest&,
        int16_t& error_code, int32_t& node_id, std::string& host, int32_t& port) {
        error_code = 0;
        node_id = config.broker_id;
        host = config.bind_address == "0.0.0.0" ? "127.0.0.1" : config.bind_address;
        port = static_cast<int32_t>(config.port);
    });

    router.set_join_group_handler([&](const strike::JoinGroupRequest& req) {
        return group_mgr.join_group(req);
    });

    router.set_sync_group_handler([&](const strike::SyncGroupRequest& req) {
        return group_mgr.sync_group(req);
    });

    router.set_heartbeat_handler([&](const strike::HeartbeatRequest& req) {
        return group_mgr.heartbeat(req);
    });

    router.set_leave_group_handler([&](const strike::LeaveGroupRequest& req) {
        return group_mgr.leave_group(req);
    });

    router.set_offset_commit_handler([&](const strike::OffsetCommitRequest& req) {
        return group_mgr.offset_commit(req);
    });

    router.set_offset_fetch_handler([&](const strike::OffsetFetchRequest& req) {
        return group_mgr.offset_fetch(req);
    });

    // Create and start HTTP admin API
    strike::http::BrokerContext http_ctx{
        config, sharded_logs, topics_mu, known_topics, group_mgr, get_or_create_log
    };
    strike::http::HttpServer http_server(http_ctx);
    g_http_server = &http_server;
    if (http_server.bind_and_listen(config.bind_address, config.http_port)) {
        http_server.start();
    }

    // Create and start TCP server (Kafka protocol)
    strike::network::TcpServer server(router, config.num_io_threads);
    g_server = &server;

    if (!server.bind_and_listen(config.bind_address, config.port)) {
        std::cerr << "Failed to start server. Exiting.\n";
        http_server.stop();
        return 1;
    }

    std::cout << "Broker ready. Press Ctrl+C to stop.\n\n" << std::flush;
    server.run();

    http_server.stop();
    std::cout << "\nStrikeMQ stopped.\n";
    return 0;
}
