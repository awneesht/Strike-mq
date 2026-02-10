#pragma once
#include "core/types.h"
#include "network/tcp_server.h"
#include "storage/partition_log.h"
#include "storage/consumer_group.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace strike {
namespace http {

// ─── Shared broker state for HTTP handlers ───

struct BrokerContext {
    BrokerConfig& config;
    std::mutex& logs_mu;
    std::unordered_map<TopicPartition, std::unique_ptr<storage::PartitionLog>,
                       TopicPartitionHash>& logs;
    std::mutex& topics_mu;
    std::vector<std::string>& known_topics;
    storage::ConsumerGroupManager& group_mgr;
    std::function<storage::PartitionLog&(const TopicPartition&)> get_or_create_log;
};

// ─── HTTP request/response types ───

struct HttpRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string, StringHash> query_params;
    std::unordered_map<std::string, std::string, StringHash> headers;
    std::string body;
};

struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::string body;
};

// ─── Minimal JSON writer ───

class JsonWriter {
public:
    void object_begin() { comma(); buf_ += '{'; need_comma_.push_back(false); }
    void object_end() { need_comma_.pop_back(); buf_ += '}'; mark_value(); }
    void array_begin() { comma(); buf_ += '['; need_comma_.push_back(false); }
    void array_end() { need_comma_.pop_back(); buf_ += ']'; mark_value(); }

    void key(const char* k) {
        comma();
        write_string(k);
        buf_ += ':';
        if (!need_comma_.empty()) need_comma_.back() = false;
    }
    void key(const std::string& k) { key(k.c_str()); }

    void value(const char* v) { comma(); write_string(v); mark_value(); }
    void value(const std::string& v) { value(v.c_str()); }
    void value(int32_t v) { comma(); buf_ += std::to_string(v); mark_value(); }
    void value(int64_t v) { comma(); buf_ += std::to_string(v); mark_value(); }
    void value(uint64_t v) { comma(); buf_ += std::to_string(v); mark_value(); }
    void value_null() { comma(); buf_ += "null"; mark_value(); }

    const std::string& str() const { return buf_; }

private:
    void comma() {
        if (!need_comma_.empty() && need_comma_.back()) buf_ += ',';
    }
    void mark_value() {
        if (!need_comma_.empty()) need_comma_.back() = true;
    }
    void write_string(const char* s) {
        buf_ += '"';
        for (; *s; ++s) {
            switch (*s) {
                case '"':  buf_ += "\\\""; break;
                case '\\': buf_ += "\\\\"; break;
                case '\n': buf_ += "\\n"; break;
                case '\r': buf_ += "\\r"; break;
                case '\t': buf_ += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(*s) < 0x20) {
                        char hex[8];
                        std::snprintf(hex, sizeof(hex), "\\u%04x",
                                      static_cast<unsigned char>(*s));
                        buf_ += hex;
                    } else {
                        buf_ += *s;
                    }
            }
        }
        buf_ += '"';
    }

    std::string buf_;
    std::vector<bool> need_comma_;
};

// ─── HTTP Server ───

class HttpServer {
public:
    explicit HttpServer(BrokerContext& ctx);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool bind_and_listen(const std::string& address, uint16_t port);
    void start();
    void stop();

private:
    void run();
    void accept_connections();
    void handle_read(int fd);
    void handle_write(int fd);
    void close_connection(int fd);
    void enable_write(int fd);
    void disable_write(int fd);
    void process_request(int fd);
    void route_request(const HttpRequest& req, HttpResponse& resp);

    // Route handlers
    void handle_get_broker(const HttpRequest& req, HttpResponse& resp);
    void handle_get_topics(const HttpRequest& req, HttpResponse& resp);
    void handle_get_topic(const HttpRequest& req, HttpResponse& resp, const std::string& name);
    void handle_delete_topic(const HttpRequest& req, HttpResponse& resp, const std::string& name);
    void handle_get_messages(const HttpRequest& req, HttpResponse& resp, const std::string& name);
    void handle_post_messages(const HttpRequest& req, HttpResponse& resp, const std::string& name);
    void handle_get_groups(const HttpRequest& req, HttpResponse& resp);
    void handle_get_group(const HttpRequest& req, HttpResponse& resp, const std::string& id);

    // JSON parsing helpers for POST
    static std::string json_extract_string(const std::string& json, const std::string& key);
    static std::vector<std::pair<std::string, std::string>> json_extract_messages(const std::string& json);

    BrokerContext& ctx_;
    int listen_fd_ = -1;
    int event_fd_ = -1;
    std::atomic<bool> running_{false};
    std::unordered_map<int, network::Connection> connections_;
    std::thread thread_;

    static constexpr int kMaxEvents = 32;
    static constexpr int kListenBacklog = 16;
    static constexpr size_t kMaxBodySize = 1048576;  // 1MB
};

} // namespace http
} // namespace strike
