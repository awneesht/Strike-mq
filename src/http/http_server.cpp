#include "http/http_server.h"
#include "utils/endian.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>

#ifdef STRIKE_PLATFORM_MACOS
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

namespace strike {
namespace http {

// ─── Startup time for uptime calculation ───

static auto g_start_time = std::chrono::steady_clock::now();

// ─── URL decode helper ───

static std::string url_decode(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = 0, lo = 0;
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            hi = hex(s[i+1]);
            lo = hex(s[i+2]);
            if (hi >= 0 && lo >= 0) {
                result += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') {
            result += ' ';
        } else {
            result += s[i];
        }
    }
    return result;
}

// ─── HTTP parsing helpers ───

static bool parse_request_line(const std::string& line, HttpRequest& req) {
    // "GET /v1/broker?foo=bar HTTP/1.1"
    auto sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    req.method = line.substr(0, sp1);
    std::string uri = line.substr(sp1 + 1, sp2 - sp1 - 1);

    auto qpos = uri.find('?');
    if (qpos != std::string::npos) {
        req.path = url_decode(uri.substr(0, qpos));
        std::string qs = uri.substr(qpos + 1);
        // Parse query params
        size_t start = 0;
        while (start < qs.size()) {
            auto amp = qs.find('&', start);
            std::string pair = (amp == std::string::npos) ?
                qs.substr(start) : qs.substr(start, amp - start);
            auto eq = pair.find('=');
            if (eq != std::string::npos) {
                req.query_params[url_decode(pair.substr(0, eq))] =
                    url_decode(pair.substr(eq + 1));
            } else {
                req.query_params[url_decode(pair)] = "";
            }
            start = (amp == std::string::npos) ? qs.size() : amp + 1;
        }
    } else {
        req.path = url_decode(uri);
    }
    return true;
}

static bool parse_headers(const std::string& header_block, HttpRequest& req) {
    size_t pos = 0;
    while (pos < header_block.size()) {
        auto eol = header_block.find("\r\n", pos);
        if (eol == std::string::npos || eol == pos) break;
        std::string line = header_block.substr(pos, eol - pos);
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            // Trim leading whitespace from value
            size_t vstart = val.find_first_not_of(' ');
            if (vstart != std::string::npos) val = val.substr(vstart);
            // Lowercase header name for easy lookup
            for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            req.headers[name] = val;
        }
        pos = eol + 2;
    }
    return true;
}

// ─── HttpServer implementation ───

HttpServer::HttpServer(BrokerContext& ctx) : ctx_(ctx) {}

HttpServer::~HttpServer() {
    stop();
    for (auto& [fd, conn] : connections_) ::close(fd);
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (event_fd_ >= 0) ::close(event_fd_);
}

bool HttpServer::bind_and_listen(const std::string& address, uint16_t port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "HTTP: socket() failed: " << std::strerror(errno) << "\n";
        return false;
    }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (!network::set_nonblocking(listen_fd_)) {
        std::cerr << "HTTP: fcntl(O_NONBLOCK) failed\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "HTTP: invalid bind address: " << address << "\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "HTTP: bind() failed: " << std::strerror(errno) << "\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, kListenBacklog) < 0) {
        std::cerr << "HTTP: listen() failed: " << std::strerror(errno) << "\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

#ifdef STRIKE_PLATFORM_MACOS
    event_fd_ = ::kqueue();
    if (event_fd_ < 0) {
        std::cerr << "HTTP: kqueue() failed\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    struct kevent ev;
    EV_SET(&ev, listen_fd_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    ::kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);
#else
    event_fd_ = ::epoll_create1(0);
    if (event_fd_ < 0) {
        std::cerr << "HTTP: epoll_create1() failed\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    ::epoll_ctl(event_fd_, EPOLL_CTL_ADD, listen_fd_, &ev);
#endif

    std::cout << "  HTTP API listening on " << address << ":" << port << "\n" << std::flush;
    return true;
}

void HttpServer::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&HttpServer::run, this);
}

void HttpServer::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void HttpServer::run() {
#ifdef STRIKE_PLATFORM_MACOS
    struct kevent events[kMaxEvents];
    while (running_.load(std::memory_order_acquire)) {
        struct timespec timeout{0, 100000000}; // 100ms
        int n = ::kevent(event_fd_, nullptr, 0, events, kMaxEvents, &timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = static_cast<int>(events[i].ident);
            if (fd == listen_fd_) {
                accept_connections();
                continue;
            }
            if (events[i].flags & (EV_EOF | EV_ERROR)) {
                close_connection(fd);
                continue;
            }
            if (events[i].filter == EVFILT_READ) {
                handle_read(fd);
            } else if (events[i].filter == EVFILT_WRITE) {
                handle_write(fd);
            }
        }
    }
#else
    struct epoll_event events[kMaxEvents];
    while (running_.load(std::memory_order_acquire)) {
        int n = ::epoll_wait(event_fd_, events, kMaxEvents, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == listen_fd_) {
                accept_connections();
                continue;
            }
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                close_connection(fd);
                continue;
            }
            if (events[i].events & EPOLLIN) {
                handle_read(fd);
            } else if (events[i].events & EPOLLOUT) {
                handle_write(fd);
            }
        }
    }
#endif
}

void HttpServer::accept_connections() {
    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd_,
            reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }

        network::set_nonblocking(client_fd);
        network::set_nodelay(client_fd);

        network::Connection conn;
        conn.fd = client_fd;
        connections_[client_fd] = std::move(conn);

#ifdef STRIKE_PLATFORM_MACOS
        struct kevent ev;
        EV_SET(&ev, client_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        ::kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);
#else
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = client_fd;
        ::epoll_ctl(event_fd_, EPOLL_CTL_ADD, client_fd, &ev);
#endif
    }
}

void HttpServer::handle_read(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    auto& conn = it->second;

    while (true) {
        auto [ptr, avail] = conn.read_buf.write_head();
        if (avail == 0) break; // buffer full
        ssize_t n = ::recv(fd, ptr, avail, 0);
        if (n > 0) {
            conn.read_buf.advance_head(static_cast<size_t>(n));
        } else if (n == 0) {
            close_connection(fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_connection(fd);
            return;
        }
    }

    // Check if we have a complete HTTP request
    process_request(fd);
}

void HttpServer::handle_write(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    auto& conn = it->second;

    while (conn.has_pending_write()) {
        auto [ptr, avail] = conn.write_buf.read_head();
        if (avail == 0) break;
        ssize_t n = ::send(fd, ptr, avail, 0);
        if (n > 0) {
            conn.write_buf.advance_tail(static_cast<size_t>(n));
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_connection(fd);
            return;
        }
    }

    if (!conn.has_pending_write()) {
        // Connection: close — done writing, close it
        close_connection(fd);
    }
}

void HttpServer::close_connection(int fd) {
#ifdef STRIKE_PLATFORM_MACOS
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(event_fd_, ev, 2, nullptr, 0, nullptr);
#else
    ::epoll_ctl(event_fd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
    ::close(fd);
    connections_.erase(fd);
}

void HttpServer::enable_write(int fd) {
#ifdef STRIKE_PLATFORM_MACOS
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    ::kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);
#else
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = fd;
    ::epoll_ctl(event_fd_, EPOLL_CTL_MOD, fd, &ev);
#endif
}

void HttpServer::disable_write(int fd) {
#ifdef STRIKE_PLATFORM_MACOS
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_WRITE, EV_DISABLE, 0, 0, nullptr);
    ::kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);
#else
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    ::epoll_ctl(event_fd_, EPOLL_CTL_MOD, fd, &ev);
#endif
}

void HttpServer::process_request(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    auto& conn = it->second;

    // Copy read buffer into a contiguous string for HTTP parsing
    size_t buf_size = conn.read_buf.size();
    std::string data(buf_size, '\0');
    conn.read_buf.peek(reinterpret_cast<uint8_t*>(data.data()), buf_size);
    auto header_end = data.find("\r\n\r\n");
    if (header_end == std::string::npos) return; // incomplete headers

    // Parse request line (first line)
    auto first_eol = data.find("\r\n");
    if (first_eol == std::string::npos) return;

    HttpRequest req;
    if (!parse_request_line(data.substr(0, first_eol), req)) {
        close_connection(fd);
        return;
    }

    // Parse headers
    parse_headers(data.substr(first_eol + 2, header_end - first_eol - 2), req);

    // Check Content-Length for body
    size_t content_length = 0;
    auto cl_it = req.headers.find("content-length");
    if (cl_it != req.headers.end()) {
        content_length = std::stoull(cl_it->second);
        if (content_length > kMaxBodySize) {
            HttpResponse resp;
            resp.status_code = 413;
            resp.status_text = "Payload Too Large";
            JsonWriter j;
            j.object_begin(); j.key("error"); j.value("Request body too large"); j.object_end();
            resp.body = j.str();
            // Serialize and send
            std::string raw = "HTTP/1.1 " + std::to_string(resp.status_code) + " " + resp.status_text + "\r\n"
                + "Content-Type: application/json\r\n"
                + "Content-Length: " + std::to_string(resp.body.size()) + "\r\n"
                + "Connection: close\r\n\r\n" + resp.body;
            conn.queue_write(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
            enable_write(fd);
            return;
        }
    }

    // Check if we have the full body
    size_t total_needed = header_end + 4 + content_length;
    if (data.size() < total_needed) return; // incomplete body

    if (content_length > 0) {
        req.body = data.substr(header_end + 4, content_length);
    }

    // Route and respond
    HttpResponse resp;
    route_request(req, resp);

    std::string raw = "HTTP/1.1 " + std::to_string(resp.status_code) + " " + resp.status_text + "\r\n"
        + "Content-Type: application/json\r\n"
        + "Content-Length: " + std::to_string(resp.body.size()) + "\r\n"
        + "Connection: close\r\n"
        + "Access-Control-Allow-Origin: *\r\n"
        + "\r\n" + resp.body;

    conn.queue_write(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    enable_write(fd);
}

// ─── Router ───

void HttpServer::route_request(const HttpRequest& req, HttpResponse& resp) {
    const std::string& path = req.path;

    // Strip trailing slash
    std::string p = path;
    if (p.size() > 1 && p.back() == '/') p.pop_back();

    // Must start with /v1/
    if (p.substr(0, 4) != "/v1/") {
        resp.status_code = 404;
        resp.status_text = "Not Found";
        JsonWriter j;
        j.object_begin(); j.key("error"); j.value("Not found"); j.object_end();
        resp.body = j.str();
        return;
    }

    std::string rest = p.substr(4); // after "/v1/"

    // GET /v1/broker
    if (rest == "broker" && req.method == "GET") {
        handle_get_broker(req, resp);
        return;
    }

    // /v1/topics...
    if (rest == "topics" && req.method == "GET") {
        handle_get_topics(req, resp);
        return;
    }

    if (rest.substr(0, 7) == "topics/") {
        std::string after = rest.substr(7); // after "topics/"

        // Check for /messages sub-path
        auto slash = after.find('/');
        if (slash != std::string::npos) {
            std::string name = after.substr(0, slash);
            std::string sub = after.substr(slash + 1);
            if (sub == "messages") {
                if (req.method == "GET") {
                    handle_get_messages(req, resp, name);
                } else if (req.method == "POST") {
                    handle_post_messages(req, resp, name);
                } else {
                    resp.status_code = 405;
                    resp.status_text = "Method Not Allowed";
                    JsonWriter j;
                    j.object_begin(); j.key("error"); j.value("Method not allowed"); j.object_end();
                    resp.body = j.str();
                }
                return;
            }
        } else {
            // /v1/topics/{name}
            std::string name = after;
            if (req.method == "GET") {
                handle_get_topic(req, resp, name);
            } else if (req.method == "DELETE") {
                handle_delete_topic(req, resp, name);
            } else {
                resp.status_code = 405;
                resp.status_text = "Method Not Allowed";
                JsonWriter j;
                j.object_begin(); j.key("error"); j.value("Method not allowed"); j.object_end();
                resp.body = j.str();
            }
            return;
        }
    }

    // /v1/groups...
    if (rest == "groups" && req.method == "GET") {
        handle_get_groups(req, resp);
        return;
    }

    if (rest.substr(0, 7) == "groups/") {
        std::string id = rest.substr(7);
        if (req.method == "GET") {
            handle_get_group(req, resp, id);
        } else {
            resp.status_code = 405;
            resp.status_text = "Method Not Allowed";
            JsonWriter j;
            j.object_begin(); j.key("error"); j.value("Method not allowed"); j.object_end();
            resp.body = j.str();
        }
        return;
    }

    resp.status_code = 404;
    resp.status_text = "Not Found";
    JsonWriter j;
    j.object_begin(); j.key("error"); j.value("Not found"); j.object_end();
    resp.body = j.str();
}

// ─── Handler implementations ───

void HttpServer::handle_get_broker(const HttpRequest& /*req*/, HttpResponse& resp) {
    auto now = std::chrono::steady_clock::now();
    auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(now - g_start_time).count();

    size_t num_io = ctx_.config.num_io_threads == 0
        ? std::max(1u, std::thread::hardware_concurrency())
        : ctx_.config.num_io_threads;

    JsonWriter j;
    j.object_begin();
    j.key("version"); j.value("0.1.4");
    j.key("broker_id"); j.value(ctx_.config.broker_id);
    j.key("uptime_seconds"); j.value(static_cast<int64_t>(uptime_s));
    j.key("port"); j.value(static_cast<int32_t>(ctx_.config.port));
    j.key("http_port"); j.value(static_cast<int32_t>(ctx_.config.http_port));
    j.key("io_threads"); j.value(static_cast<int64_t>(num_io));
#ifdef STRIKE_PLATFORM_MACOS
    j.key("platform"); j.value("macOS (kqueue)");
#else
    j.key("platform"); j.value("Linux (epoll)");
#endif
    j.key("log_dir"); j.value(ctx_.config.log_dir);
    j.key("segment_size"); j.value(ctx_.config.segment_size);
    j.object_end();
    resp.body = j.str();
}

void HttpServer::handle_get_topics(const HttpRequest& /*req*/, HttpResponse& resp) {
    // Collect topic info via sharded iteration
    struct TopicInfo {
        int32_t partitions = 0;
        int64_t min_offset = 0;
        int64_t max_offset = 0;
    };
    std::unordered_map<std::string, TopicInfo, StringHash> topics;

    ctx_.sharded_logs.for_each([&](const TopicPartition& tp, storage::PartitionLog& log) {
        auto& info = topics[tp.topic];
        info.partitions++;
        int64_t start = log.start_offset();
        int64_t end = log.next_offset();
        if (info.partitions == 1) {
            info.min_offset = start;
            info.max_offset = end;
        } else {
            info.min_offset = std::min(info.min_offset, start);
            info.max_offset = std::max(info.max_offset, end);
        }
    });

    JsonWriter j;
    j.array_begin();
    for (const auto& [name, info] : topics) {
        j.object_begin();
        j.key("name"); j.value(name);
        j.key("partitions"); j.value(info.partitions);
        j.key("start_offset"); j.value(info.min_offset);
        j.key("end_offset"); j.value(info.max_offset);
        j.object_end();
    }
    j.array_end();
    resp.body = j.str();
}

void HttpServer::handle_get_topic(const HttpRequest& /*req*/, HttpResponse& resp, const std::string& name) {
    // Find all partitions for this topic via sharded iteration
    struct PartInfo {
        int32_t partition;
        int64_t start_offset;
        int64_t end_offset;
    };
    std::vector<PartInfo> parts;

    ctx_.sharded_logs.for_each([&](const TopicPartition& tp, storage::PartitionLog& log) {
        if (tp.topic == name) {
            PartInfo p;
            p.partition = tp.partition;
            p.start_offset = log.start_offset();
            p.end_offset = log.next_offset();
            parts.push_back(p);
        }
    });

    if (parts.empty()) {
        resp.status_code = 404;
        resp.status_text = "Not Found";
        JsonWriter j;
        j.object_begin(); j.key("error"); j.value("Topic not found"); j.object_end();
        resp.body = j.str();
        return;
    }

    std::sort(parts.begin(), parts.end(), [](const PartInfo& a, const PartInfo& b) {
        return a.partition < b.partition;
    });

    JsonWriter j;
    j.object_begin();
    j.key("name"); j.value(name);
    j.key("partitions");
    j.array_begin();
    for (const auto& p : parts) {
        j.object_begin();
        j.key("partition"); j.value(p.partition);
        j.key("start_offset"); j.value(p.start_offset);
        j.key("end_offset"); j.value(p.end_offset);
        j.object_end();
    }
    j.array_end();
    j.object_end();
    resp.body = j.str();
}

void HttpServer::handle_delete_topic(const HttpRequest& /*req*/, HttpResponse& resp, const std::string& name) {
    // Erase matching partitions from sharded map
    auto erased = ctx_.sharded_logs.erase_if([&](const TopicPartition& tp, storage::PartitionLog&) {
        return tp.topic == name;
    });

    if (erased.empty()) {
        resp.status_code = 404;
        resp.status_text = "Not Found";
        JsonWriter j;
        j.object_begin(); j.key("error"); j.value("Topic not found"); j.object_end();
        resp.body = j.str();
        return;
    }

    // Delete data directories
    for (const auto& tp : erased) {
        std::string dir = ctx_.config.log_dir + "/" + tp.topic + "-" + std::to_string(tp.partition);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // Remove from known_topics
    {
        std::lock_guard<std::mutex> tlock(ctx_.topics_mu);
        ctx_.known_topics.erase(
            std::remove(ctx_.known_topics.begin(), ctx_.known_topics.end(), name),
            ctx_.known_topics.end());
    }

    JsonWriter j;
    j.object_begin();
    j.key("deleted"); j.value(name);
    j.key("partitions_removed"); j.value(static_cast<int32_t>(erased.size()));
    j.object_end();
    resp.body = j.str();
}

void HttpServer::handle_get_messages(const HttpRequest& req, HttpResponse& resp, const std::string& name) {
    int64_t offset = 0;
    int32_t limit = 10;
    int32_t partition = 0;

    auto it = req.query_params.find("offset");
    if (it != req.query_params.end()) offset = std::stoll(it->second);
    it = req.query_params.find("limit");
    if (it != req.query_params.end()) limit = std::stoi(it->second);
    it = req.query_params.find("partition");
    if (it != req.query_params.end()) partition = std::stoi(it->second);

    if (limit <= 0) limit = 10;
    if (limit > 1000) limit = 1000;

    TopicPartition tp{name, partition};

    auto* log_ptr = ctx_.sharded_logs.find(tp);
    if (!log_ptr) {
        resp.status_code = 404;
        resp.status_text = "Not Found";
        JsonWriter j;
        j.object_begin(); j.key("error"); j.value("Topic/partition not found"); j.object_end();
        resp.body = j.str();
        return;
    }

    auto& log = *log_ptr;
    auto result = log.read(offset, limit * 65536); // generous max_bytes

    JsonWriter j;
    j.array_begin();

    if (result.data && result.size > 0) {
        const uint8_t* seg = result.data;
        uint32_t seg_size = result.size;
        uint32_t scan = 0;
        int32_t count = 0;

        while (scan + 12 <= seg_size && count < limit) {
            // Read baseOffset (8 bytes BE) + batchLength (4 bytes BE)
            uint64_t bo_raw;
            std::memcpy(&bo_raw, seg + scan, 8);
            int64_t batch_base = static_cast<int64_t>(strike::bswap64(bo_raw));

            uint32_t bl_raw;
            std::memcpy(&bl_raw, seg + scan + 8, 4);
            int32_t batch_len = static_cast<int32_t>(strike::bswap32(bl_raw));

            if (batch_len <= 0) break;
            uint32_t total = 12 + static_cast<uint32_t>(batch_len);
            if (scan + total > seg_size) break;

            // Parse batch header to get record count
            // Batch header layout after baseOffset(8) + batchLength(4):
            //   partitionLeaderEpoch(4) + magic(1) + crc(4) + attributes(2) +
            //   lastOffsetDelta(4) + firstTimestamp(8) + maxTimestamp(8) +
            //   producerId(8) + producerEpoch(2) + baseSequence(4) + recordCount(4)
            // = 49 bytes from offset 12
            if (total < 61) { scan += total; continue; } // too small

            uint32_t rc_raw;
            std::memcpy(&rc_raw, seg + scan + 57, 4);
            int32_t record_count = static_cast<int32_t>(strike::bswap32(rc_raw));

            // Read firstTimestamp
            uint64_t ts_raw;
            std::memcpy(&ts_raw, seg + scan + 25, 8);
            int64_t first_timestamp = static_cast<int64_t>(strike::bswap64(ts_raw));

            // Parse individual records starting at offset 61
            uint32_t rpos = scan + 61;
            for (int32_t r = 0; r < record_count && count < limit && rpos < scan + total; ++r) {
                // Each record starts with:
                //   length (varint), attributes (1 byte), timestampDelta (varint),
                //   offsetDelta (varint), keyLength (varint), key (keyLength bytes),
                //   valueLength (varint), value (valueLength bytes), headerCount (varint)

                auto read_varint = [&](uint32_t& pos) -> int64_t {
                    int64_t result = 0;
                    int shift = 0;
                    while (pos < scan + total) {
                        uint8_t b = seg[pos++];
                        result |= static_cast<int64_t>(b & 0x7F) << shift;
                        if ((b & 0x80) == 0) break;
                        shift += 7;
                    }
                    // Zigzag decode
                    return static_cast<int64_t>((static_cast<uint64_t>(result) >> 1) ^
                        -(static_cast<uint64_t>(result) & 1));
                };

                int64_t rec_length = read_varint(rpos);
                (void)rec_length;
                if (rpos >= scan + total) break;

                // attributes (1 byte)
                rpos++;
                if (rpos >= scan + total) break;

                int64_t timestamp_delta = read_varint(rpos);
                int64_t offset_delta = read_varint(rpos);
                int64_t key_length = read_varint(rpos);

                std::string key_str;
                if (key_length > 0 && rpos + static_cast<uint32_t>(key_length) <= scan + total) {
                    key_str.assign(reinterpret_cast<const char*>(seg + rpos),
                                   static_cast<size_t>(key_length));
                    rpos += static_cast<uint32_t>(key_length);
                } else if (key_length > 0) {
                    break;
                }

                int64_t value_length = read_varint(rpos);
                std::string value_str;
                if (value_length > 0 && rpos + static_cast<uint32_t>(value_length) <= scan + total) {
                    value_str.assign(reinterpret_cast<const char*>(seg + rpos),
                                     static_cast<size_t>(value_length));
                    rpos += static_cast<uint32_t>(value_length);
                } else if (value_length > 0) {
                    break;
                }

                // Skip header count
                read_varint(rpos);

                j.object_begin();
                j.key("offset"); j.value(batch_base + offset_delta);
                j.key("timestamp"); j.value(first_timestamp + timestamp_delta);
                if (!key_str.empty()) {
                    j.key("key"); j.value(key_str);
                } else {
                    j.key("key"); j.value_null();
                }
                j.key("value"); j.value(value_str);
                j.object_end();
                count++;
            }

            scan += total;
        }
    }

    j.array_end();
    resp.body = j.str();
}

// ─── Minimal JSON parsing for POST /messages ───

std::string HttpServer::json_extract_string(const std::string& json, const std::string& key) {
    // Find "key": "value" in JSON string. Very simple, no nesting support needed.
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();

    // Skip whitespace and colon
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++; // skip opening quote

    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        pos++;
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> HttpServer::json_extract_messages(const std::string& json) {
    // Parse {"messages": [{"key": "k", "value": "v"}, ...]}
    // Simple state machine: find each {...} inside "messages": [...]
    std::vector<std::pair<std::string, std::string>> messages;

    auto arr_start = json.find("\"messages\"");
    if (arr_start == std::string::npos) return messages;

    // Find the opening [
    auto bracket = json.find('[', arr_start);
    if (bracket == std::string::npos) return messages;

    size_t pos = bracket + 1;
    while (pos < json.size()) {
        // Find next {
        auto obj_start = json.find('{', pos);
        if (obj_start == std::string::npos) break;

        // Find matching }
        int depth = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < json.size() && depth > 0) {
            if (json[obj_end] == '{') depth++;
            else if (json[obj_end] == '}') depth--;
            if (depth > 0) obj_end++;
        }
        if (depth != 0) break;

        std::string obj = json.substr(obj_start, obj_end - obj_start + 1);
        std::string key = json_extract_string(obj, "key");
        std::string value = json_extract_string(obj, "value");
        messages.emplace_back(std::move(key), std::move(value));

        pos = obj_end + 1;
    }

    return messages;
}

void HttpServer::handle_post_messages(const HttpRequest& req, HttpResponse& resp, const std::string& name) {
    if (req.body.empty()) {
        resp.status_code = 400;
        resp.status_text = "Bad Request";
        JsonWriter j;
        j.object_begin(); j.key("error"); j.value("Empty request body"); j.object_end();
        resp.body = j.str();
        return;
    }

    auto messages = json_extract_messages(req.body);
    if (messages.empty()) {
        resp.status_code = 400;
        resp.status_text = "Bad Request";
        JsonWriter j;
        j.object_begin(); j.key("error"); j.value("No messages found in body"); j.object_end();
        resp.body = j.str();
        return;
    }

    int32_t partition = 0;
    auto it = req.query_params.find("partition");
    if (it != req.query_params.end()) partition = std::stoi(it->second);

    TopicPartition tp{name, partition};

    // Build a RecordBatch
    RecordBatch batch;
    batch.topic_partition = tp;
    batch.first_timestamp = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    batch.max_timestamp = batch.first_timestamp;

    // We need to build record data that the PartitionLog can serialize
    // Each record in Kafka batch format:
    //   length (varint) | attributes (1) | timestampDelta (varint) | offsetDelta (varint) |
    //   keyLength (varint) | key | valueLength (varint) | value | headerCount (varint)
    std::vector<std::vector<uint8_t>> record_bufs;

    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& [key, value] = messages[i];
        std::vector<uint8_t> rbuf;

        auto write_varint = [&](int64_t v) {
            // Zigzag encode
            uint64_t zz = (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
            while (zz > 0x7F) {
                rbuf.push_back(static_cast<uint8_t>((zz & 0x7F) | 0x80));
                zz >>= 7;
            }
            rbuf.push_back(static_cast<uint8_t>(zz));
        };

        // Build record content first (without length prefix)
        std::vector<uint8_t> content;
        auto write_varint_to = [&](std::vector<uint8_t>& buf, int64_t v) {
            uint64_t zz = (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
            while (zz > 0x7F) {
                buf.push_back(static_cast<uint8_t>((zz & 0x7F) | 0x80));
                zz >>= 7;
            }
            buf.push_back(static_cast<uint8_t>(zz));
        };

        content.push_back(0); // attributes
        write_varint_to(content, 0); // timestampDelta
        write_varint_to(content, static_cast<int64_t>(i)); // offsetDelta

        if (key.empty()) {
            write_varint_to(content, -1); // null key
        } else {
            write_varint_to(content, static_cast<int64_t>(key.size()));
            content.insert(content.end(), key.begin(), key.end());
        }

        write_varint_to(content, static_cast<int64_t>(value.size()));
        content.insert(content.end(), value.begin(), value.end());
        write_varint_to(content, 0); // header count

        // Now write length prefix + content
        write_varint(static_cast<int64_t>(content.size()));
        rbuf.insert(rbuf.end(), content.begin(), content.end());

        record_bufs.push_back(std::move(rbuf));
    }

    // Build records pointing to our buffers
    for (size_t i = 0; i < record_bufs.size(); ++i) {
        Record r;
        r.data = record_bufs[i].data();
        r.total_size = static_cast<uint32_t>(record_bufs[i].size());
        // key/value offsets not needed since we're writing raw pre-encoded data
        batch.records.push_back(r);
    }

    auto& log = ctx_.get_or_create_log(tp);
    int64_t base_offset = log.append(batch);

    JsonWriter j;
    j.object_begin();
    j.key("topic"); j.value(name);
    j.key("partition"); j.value(partition);
    j.key("base_offset"); j.value(base_offset);
    j.key("record_count"); j.value(static_cast<int32_t>(messages.size()));
    j.object_end();
    resp.body = j.str();
}

void HttpServer::handle_get_groups(const HttpRequest& /*req*/, HttpResponse& resp) {
    auto groups = ctx_.group_mgr.list_groups();
    JsonWriter j;
    j.array_begin();
    for (const auto& g : groups) {
        j.object_begin();
        j.key("group_id"); j.value(g.group_id);
        j.key("state"); j.value(g.state);
        j.key("generation_id"); j.value(g.generation_id);
        j.key("member_count"); j.value(static_cast<int64_t>(g.member_count));
        j.object_end();
    }
    j.array_end();
    resp.body = j.str();
}

void HttpServer::handle_get_group(const HttpRequest& /*req*/, HttpResponse& resp, const std::string& id) {
    auto detail = ctx_.group_mgr.get_group(id);
    if (!detail) {
        resp.status_code = 404;
        resp.status_text = "Not Found";
        JsonWriter j;
        j.object_begin(); j.key("error"); j.value("Consumer group not found"); j.object_end();
        resp.body = j.str();
        return;
    }

    JsonWriter j;
    j.object_begin();
    j.key("group_id"); j.value(detail->group_id);
    j.key("state"); j.value(detail->state);
    j.key("generation_id"); j.value(detail->generation_id);
    j.key("protocol_type"); j.value(detail->protocol_type);
    j.key("protocol_name"); j.value(detail->protocol_name);
    j.key("leader_id"); j.value(detail->leader_id);

    j.key("members");
    j.array_begin();
    for (const auto& m : detail->members) {
        j.object_begin();
        j.key("member_id"); j.value(m.member_id);
        j.key("client_id"); j.value(m.client_id);
        j.key("session_timeout_ms"); j.value(m.session_timeout_ms);
        j.key("rebalance_timeout_ms"); j.value(m.rebalance_timeout_ms);
        j.object_end();
    }
    j.array_end();

    j.key("committed_offsets");
    j.object_begin();
    for (const auto& [tp, off] : detail->committed_offsets) {
        std::string key = tp.topic + "-" + std::to_string(tp.partition);
        j.key(key); j.value(off);
    }
    j.object_end();

    j.object_end();
    resp.body = j.str();
}

} // namespace http
} // namespace strike
