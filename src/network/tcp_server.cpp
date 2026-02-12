#include "network/tcp_server.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>

#ifdef STRIKE_IO_URING
#include "network/io_uring_defs.h"
#include <liburing.h>
#endif

namespace strike {
namespace network {

// ─── Free functions ───

bool set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool set_nodelay(int fd) {
    int yes = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0;
}

// ─── WorkerThread ───

WorkerThread::WorkerThread(protocol::RequestRouter& router, int id)
    : router_(router), id_(id) {}

WorkerThread::~WorkerThread() {
    stop();
    for (auto& [fd, conn] : connections_) ::close(fd);
    if (event_fd_ >= 0) ::close(event_fd_);
    if (wakeup_pipe_[0] >= 0) ::close(wakeup_pipe_[0]);
    if (wakeup_pipe_[1] >= 0) ::close(wakeup_pipe_[1]);
}

void WorkerThread::start() {
    if (::pipe(wakeup_pipe_) < 0) {
        std::cerr << "Worker " << id_ << ": pipe() failed: " << std::strerror(errno) << "\n";
        return;
    }
    set_nonblocking(wakeup_pipe_[0]);
    set_nonblocking(wakeup_pipe_[1]);

#ifdef STRIKE_IO_URING
    // io_uring setup deferred to run_io_uring(); event_fd_ not used
#elif defined(STRIKE_PLATFORM_MACOS)
    event_fd_ = ::kqueue();
    if (event_fd_ < 0) {
        std::cerr << "Worker " << id_ << ": kqueue() failed: " << std::strerror(errno) << "\n";
        return;
    }
    struct kevent ev;
    EV_SET(&ev, wakeup_pipe_[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    ::kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);
#else
    event_fd_ = ::epoll_create1(0);
    if (event_fd_ < 0) {
        std::cerr << "Worker " << id_ << ": epoll_create1() failed: " << std::strerror(errno) << "\n";
        return;
    }
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wakeup_pipe_[0];
    ::epoll_ctl(event_fd_, EPOLL_CTL_ADD, wakeup_pipe_[0], &ev);
#endif

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&WorkerThread::run, this);
}

void WorkerThread::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    // Wake the thread so it exits the event loop
    if (wakeup_pipe_[1] >= 0) {
        uint8_t byte = 1;
        [[maybe_unused]] auto _ = ::write(wakeup_pipe_[1], &byte, 1);
    }
    if (thread_.joinable()) thread_.join();
}

bool WorkerThread::assign_connection(int fd) {
    if (!new_fds_.try_push(fd)) return false;
    // Wake worker to drain the ring buffer
    uint8_t byte = 1;
    [[maybe_unused]] auto _ = ::write(wakeup_pipe_[1], &byte, 1);
    return true;
}

void WorkerThread::drain_new_connections() {
    // Drain wakeup pipe bytes
    uint8_t drain_buf[128];
    while (::read(wakeup_pipe_[0], drain_buf, sizeof(drain_buf)) > 0) {}

    // Pop new fds from ring buffer and register with kqueue/epoll
    int fds[64];
    size_t count = new_fds_.try_pop_batch(fds, 64);
    for (size_t i = 0; i < count; ++i) {
        int fd = fds[i];
        connections_.emplace(std::piecewise_construct,
                             std::forward_as_tuple(fd),
                             std::forward_as_tuple());
        connections_[fd].fd = fd;

#ifdef STRIKE_PLATFORM_MACOS
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        ::kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);
#elif !defined(STRIKE_IO_URING)
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        ::epoll_ctl(event_fd_, EPOLL_CTL_ADD, fd, &ev);
#endif
    }
}

void WorkerThread::run() {
#ifdef STRIKE_IO_URING
    run_io_uring();
#elif defined(STRIKE_PLATFORM_MACOS)
    struct kevent events[kMaxEvents];
    while (running_.load(std::memory_order_acquire)) {
        struct timespec timeout{0, 1000000}; // 1ms
        int n = ::kevent(event_fd_, nullptr, 0, events, kMaxEvents, &timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "Worker " << id_ << ": kevent() error: " << std::strerror(errno) << "\n";
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = static_cast<int>(events[i].ident);
            if (fd == wakeup_pipe_[0]) {
                drain_new_connections();
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
        int n = ::epoll_wait(event_fd_, events, kMaxEvents, 1); // 1ms timeout
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "Worker " << id_ << ": epoll_wait() error: " << std::strerror(errno) << "\n";
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == wakeup_pipe_[0]) {
                drain_new_connections();
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

void WorkerThread::handle_read(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    Connection& conn = it->second;

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

    process_frames(conn);
}

void WorkerThread::process_frames(Connection& conn) {
    while (conn.read_buf.size() >= 4) {
        // Peek at frame size (4 bytes, may span wrap boundary)
        uint8_t hdr[4];
        conn.read_buf.peek(hdr, 4);
        uint32_t frame_size = strike::bswap32(*reinterpret_cast<uint32_t*>(hdr));

        if (frame_size > kMaxFrameSize) {
            std::cerr << "  Frame too large (" << frame_size << " bytes), closing connection\n";
            close_connection(conn.fd);
            return;
        }

        size_t total = 4 + frame_size;
        if (conn.read_buf.size() < total) break;

        // Read the complete frame into a contiguous buffer
        // Check if contiguous in the ring first (common case)
        auto [rptr, contig] = conn.read_buf.read_head();

        uint8_t frame_stack[65536];
        uint8_t* frame_data;
        bool used_heap = false;

        if (contig >= total) {
            // Fast path: frame is contiguous in the ring
            frame_data = const_cast<uint8_t*>(rptr) + 4;
        } else {
            // Slow path: frame wraps around — copy to temp buffer
            uint8_t* buf = frame_stack;
            if (total > sizeof(frame_stack)) {
                buf = new uint8_t[total];
                used_heap = true;
            }
            conn.read_buf.peek(buf, total);
            frame_data = buf + 4;
        }

        uint8_t resp_buf[65536];
        size_t resp_len = router_.route_request(
            frame_data, frame_size, resp_buf, sizeof(resp_buf));

        if (used_heap && frame_data != frame_stack + 4) {
            delete[] (frame_data - 4);
        }

        if (resp_len > 0) {
            conn.queue_write(resp_buf, resp_len);
            enable_write(conn.fd);
        }

        conn.read_buf.advance_tail(total);
    }
}

void WorkerThread::handle_write(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    Connection& conn = it->second;

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
        disable_write(fd);
    }
}

void WorkerThread::close_connection(int fd) {
#ifdef STRIKE_PLATFORM_MACOS
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(event_fd_, ev, 2, nullptr, 0, nullptr);
#elif !defined(STRIKE_IO_URING)
    ::epoll_ctl(event_fd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
    ::close(fd);
    connections_.erase(fd);
    std::cout << "  - Connection closed (fd=" << fd << ", worker=" << id_ << ")\n" << std::flush;
}

void WorkerThread::enable_write(int fd) {
#ifdef STRIKE_PLATFORM_MACOS
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    ::kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);
#elif !defined(STRIKE_IO_URING)
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = fd;
    ::epoll_ctl(event_fd_, EPOLL_CTL_MOD, fd, &ev);
#endif
}

void WorkerThread::disable_write(int fd) {
#ifdef STRIKE_PLATFORM_MACOS
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_WRITE, EV_DISABLE, 0, 0, nullptr);
    ::kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);
#elif !defined(STRIKE_IO_URING)
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    ::epoll_ctl(event_fd_, EPOLL_CTL_MOD, fd, &ev);
#endif
}

// ─── io_uring worker event loop (Linux only) ───

#ifdef STRIKE_IO_URING

void WorkerThread::drain_new_connections_uring() {
    // Drain wakeup pipe bytes
    uint8_t drain_buf[128];
    while (::read(wakeup_pipe_[0], drain_buf, sizeof(drain_buf)) > 0) {}

    int fds[64];
    size_t count = new_fds_.try_pop_batch(fds, 64);
    for (size_t i = 0; i < count; ++i) {
        int fd = fds[i];
        connections_.emplace(std::piecewise_construct,
                             std::forward_as_tuple(fd),
                             std::forward_as_tuple());
        connections_[fd].fd = fd;
        // Submit initial recv for this connection
        submit_recv(connections_[fd]);
    }
}

void WorkerThread::submit_recv(Connection& conn) {
    // Will be submitted when io_uring_submit() is called
    // For now this is a placeholder — actual SQE submission
    // requires the ring pointer which is stack-local in run_io_uring()
    (void)conn;
}

void WorkerThread::submit_send(Connection& conn) {
    (void)conn;
}

void WorkerThread::handle_recv_completion(int fd, int res) {
    if (res <= 0) {
        close_connection(fd);
        return;
    }
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    auto& conn = it->second;
    conn.read_buf.advance_head(static_cast<size_t>(res));
    process_frames(conn);
}

void WorkerThread::handle_send_completion(int fd, int res) {
    if (res <= 0) {
        close_connection(fd);
        return;
    }
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    auto& conn = it->second;
    conn.write_buf.advance_tail(static_cast<size_t>(res));
}

void WorkerThread::run_io_uring() {
    struct io_uring ring;
    struct io_uring_params params{};
    params.flags = IORING_SETUP_SINGLE_ISSUER;
    // Try SQPOLL first — falls back to normal if insufficient privileges
    params.flags |= IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 1000; // 1ms idle before SQPOLL thread sleeps

    int ret = io_uring_queue_init_params(256, &ring, &params);
    if (ret < 0) {
        // Fallback without SQPOLL
        std::memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_SINGLE_ISSUER;
        ret = io_uring_queue_init_params(256, &ring, &params);
        if (ret < 0) {
            std::cerr << "Worker " << id_ << ": io_uring_queue_init failed: "
                      << std::strerror(-ret) << "\n";
            return;
        }
    }

    // Register wakeup pipe for new connection notifications
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (sqe) {
        io_uring_prep_read(sqe, wakeup_pipe_[0], nullptr, 0, 0);
        // We use a special poll for the wakeup pipe
        io_uring_prep_poll_add(sqe, wakeup_pipe_[0], POLLIN);
        io_uring_sqe_set_data64(sqe, encode_userdata(UringOp::Accept, wakeup_pipe_[0]));
    }
    io_uring_submit(&ring);

    struct __kernel_timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000; // 1ms

    while (running_.load(std::memory_order_acquire)) {
        // Drain new connections
        drain_new_connections_uring();

        // Submit initial recvs for newly added connections
        for (auto& [fd, conn] : connections_) {
            // Submit recv if we have space in read buffer
            auto [ptr, avail] = conn.read_buf.write_head();
            if (avail > 0) {
                sqe = io_uring_get_sqe(&ring);
                if (sqe) {
                    io_uring_prep_recv(sqe, fd, ptr, avail, 0);
                    io_uring_sqe_set_data64(sqe, encode_userdata(UringOp::Recv, fd));
                }
            }
            // Submit send if we have pending writes
            if (conn.has_pending_write()) {
                auto [sptr, savail] = conn.write_buf.read_head();
                if (savail > 0) {
                    sqe = io_uring_get_sqe(&ring);
                    if (sqe) {
                        io_uring_prep_send(sqe, fd, sptr, savail, 0);
                        io_uring_sqe_set_data64(sqe, encode_userdata(UringOp::Send, fd));
                    }
                }
            }
        }
        io_uring_submit(&ring);

        // Reap completions
        struct io_uring_cqe* cqe;
        ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
        if (ret == -ETIME || ret == -EINTR) continue;
        if (ret < 0) break;

        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring, head, cqe) {
            uint64_t userdata = io_uring_cqe_get_data64(cqe);
            UringOp op = decode_op(userdata);
            int fd = decode_fd(userdata);

            switch (op) {
            case UringOp::Accept:
                // Wakeup pipe notification — drain in next iteration
                // Re-arm poll
                sqe = io_uring_get_sqe(&ring);
                if (sqe) {
                    io_uring_prep_poll_add(sqe, wakeup_pipe_[0], POLLIN);
                    io_uring_sqe_set_data64(sqe, encode_userdata(UringOp::Accept, wakeup_pipe_[0]));
                }
                break;
            case UringOp::Recv:
                handle_recv_completion(fd, cqe->res);
                break;
            case UringOp::Send:
                handle_send_completion(fd, cqe->res);
                break;
            case UringOp::Close:
                connections_.erase(fd);
                break;
            }
            count++;
        }
        io_uring_cq_advance(&ring, count);
    }

    io_uring_queue_exit(&ring);
}

#endif // STRIKE_IO_URING

// ─── TcpServer (acceptor-only) ───

TcpServer::TcpServer(protocol::RequestRouter& router, size_t num_workers)
    : router_(router)
    , num_workers_(num_workers == 0
        ? std::max(1u, std::thread::hardware_concurrency())
        : num_workers)
{}

TcpServer::~TcpServer() {
    stop();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (event_fd_ >= 0) ::close(event_fd_);
}

bool TcpServer::bind_and_listen(const std::string& address, uint16_t port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
        return false;
    }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    if (!set_nonblocking(listen_fd_)) {
        std::cerr << "fcntl(O_NONBLOCK) failed on listen socket\n";
        return false;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "Invalid bind address: " << address << "\n";
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed: " << std::strerror(errno) << "\n";
        return false;
    }

    if (::listen(listen_fd_, kListenBacklog) < 0) {
        std::cerr << "listen() failed: " << std::strerror(errno) << "\n";
        return false;
    }

#ifdef STRIKE_PLATFORM_MACOS
    event_fd_ = ::kqueue();
    if (event_fd_ < 0) {
        std::cerr << "kqueue() failed: " << std::strerror(errno) << "\n";
        return false;
    }
    struct kevent ev;
    EV_SET(&ev, listen_fd_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    if (::kevent(event_fd_, &ev, 1, nullptr, 0, nullptr) < 0) {
        std::cerr << "kevent(listen) failed: " << std::strerror(errno) << "\n";
        return false;
    }
#elif !defined(STRIKE_IO_URING)
    event_fd_ = ::epoll_create1(0);
    if (event_fd_ < 0) {
        std::cerr << "epoll_create1() failed: " << std::strerror(errno) << "\n";
        return false;
    }
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (::epoll_ctl(event_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        std::cerr << "epoll_ctl(listen) failed: " << std::strerror(errno) << "\n";
        return false;
    }
#endif

    std::cout << "  Listening on " << address << ":" << port << "\n" << std::flush;
    return true;
}

void TcpServer::run() {
    // Create and start worker threads
    workers_.reserve(num_workers_);
    for (size_t i = 0; i < num_workers_; ++i) {
        auto worker = std::make_unique<WorkerThread>(router_, static_cast<int>(i));
        worker->start();
        workers_.push_back(std::move(worker));
    }

    running_.store(true, std::memory_order_release);

    // Accept-only event loop
#ifdef STRIKE_IO_URING
    // io_uring accept loop — use simple polling since accept is infrequent
    while (running_.load(std::memory_order_acquire)) {
        accept_connections();
        // Brief sleep to avoid spinning — accept is infrequent
        struct timespec ts{0, 1000000}; // 1ms
        ::nanosleep(&ts, nullptr);
    }
#elif defined(STRIKE_PLATFORM_MACOS)
    struct kevent events[kMaxEvents];
    while (running_.load(std::memory_order_acquire)) {
        struct timespec timeout{0, 1000000}; // 1ms
        int n = ::kevent(event_fd_, nullptr, 0, events, kMaxEvents, &timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "kevent() error: " << std::strerror(errno) << "\n";
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (static_cast<int>(events[i].ident) == listen_fd_) {
                accept_connections();
            }
        }
    }
#else
    struct epoll_event events[kMaxEvents];
    while (running_.load(std::memory_order_acquire)) {
        int n = ::epoll_wait(event_fd_, events, kMaxEvents, 1); // 1ms timeout
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait() error: " << std::strerror(errno) << "\n";
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == listen_fd_) {
                accept_connections();
            }
        }
    }
#endif

    // Stop all workers on exit
    for (auto& w : workers_) w->stop();
    workers_.clear();
}

void TcpServer::stop() {
    running_.store(false, std::memory_order_release);
}

void TcpServer::accept_connections() {
    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd_,
            reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            std::cerr << "accept() failed: " << std::strerror(errno) << "\n";
            break;
        }

        set_nonblocking(client_fd);
        set_nodelay(client_fd);

#ifdef SO_BUSY_POLL
        // SO_BUSY_POLL reduces wakeup latency from interrupt-driven ~10-50us
        // to polling ~1-5us on supported kernels
        {
            int busy_poll_us = 50; // microseconds
            ::setsockopt(client_fd, SOL_SOCKET, SO_BUSY_POLL,
                         &busy_poll_us, sizeof(busy_poll_us));
        }
#endif

        // Round-robin assignment to workers
        bool assigned = false;
        size_t start = next_worker_;
        for (size_t attempt = 0; attempt < num_workers_; ++attempt) {
            size_t idx = (start + attempt) % num_workers_;
            if (workers_[idx]->assign_connection(client_fd)) {
                next_worker_ = (idx + 1) % num_workers_;
                assigned = true;
                break;
            }
        }

        if (!assigned) {
            std::cerr << "  All worker ring buffers full, dropping connection\n";
            ::close(client_fd);
            continue;
        }

        char addr_str[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
        std::cout << "  + Connection from " << addr_str
                  << ":" << ntohs(client_addr.sin_port)
                  << " → worker " << ((next_worker_ + num_workers_ - 1) % num_workers_)
                  << "\n" << std::flush;
    }
}

} // namespace network
} // namespace strike
