#pragma once
#include "core/types.h"
#include "protocol/kafka_codec.h"
#include "utils/circular_buffer.h"
#include "utils/endian.h"
#include "utils/ring_buffer.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef STRIKE_PLATFORM_MACOS
#include <sys/event.h>
#elif defined(STRIKE_IO_URING)
// io_uring headers included only in .cpp
#else
#include <sys/epoll.h>
#endif

namespace strike {
namespace network {

bool set_nonblocking(int fd);
bool set_nodelay(int fd);

struct Connection {
    int fd = -1;
    FixedCircularBuffer read_buf{131072};   // 128KB
    FixedCircularBuffer write_buf{131072};  // 128KB

    Connection() = default;
    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;

    void queue_write(const uint8_t* data, size_t len) {
        write_buf.write(data, len);
    }

    bool has_pending_write() const {
        return !write_buf.empty();
    }
};

class WorkerThread {
public:
    explicit WorkerThread(protocol::RequestRouter& router, int id);
    ~WorkerThread();

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    void start();
    void stop();
    bool assign_connection(int fd);

private:
    void run();
    void drain_new_connections();
    void handle_read(int fd);
    void handle_write(int fd);
    void process_frames(Connection& conn);
    void close_connection(int fd);
    void enable_write(int fd);
    void disable_write(int fd);

#ifdef STRIKE_IO_URING
    // io_uring-specific methods
    void run_io_uring();
    void drain_new_connections_uring();
    void submit_recv(Connection& conn);
    void submit_send(Connection& conn);
    void handle_recv_completion(int fd, int res);
    void handle_send_completion(int fd, int res);
#endif

    protocol::RequestRouter& router_;
    int id_;
    int event_fd_ = -1;
    int wakeup_pipe_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::unordered_map<int, Connection> connections_;
    SPSCRingBuffer<int, 1024> new_fds_;
    std::thread thread_;

#ifdef STRIKE_IO_URING
    // io_uring instance is stored as opaque pointer to avoid header pollution;
    // actual struct io_uring is stack-allocated in run_io_uring()
#endif

    static constexpr size_t kReadChunk = 65536;
    static constexpr size_t kMaxFrameSize = 104857600; // 100MB
    static constexpr int kMaxEvents = 64;
};

class TcpServer {
public:
    explicit TcpServer(protocol::RequestRouter& router, size_t num_workers = 0);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool bind_and_listen(const std::string& address, uint16_t port);
    void run();
    void stop();

private:
    void accept_connections();

    protocol::RequestRouter& router_;
    int listen_fd_ = -1;
    int event_fd_ = -1;
    std::atomic<bool> running_{false};
    std::vector<std::unique_ptr<WorkerThread>> workers_;
    size_t next_worker_ = 0;
    size_t num_workers_;

    static constexpr int kMaxEvents = 64;
    static constexpr int kListenBacklog = 128;
};

} // namespace network
} // namespace strike
