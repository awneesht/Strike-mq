#include "network/tcp_server.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>

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

#ifdef STRIKE_PLATFORM_MACOS
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
        Connection conn;
        conn.fd = fd;
        connections_[fd] = std::move(conn);

#ifdef STRIKE_PLATFORM_MACOS
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        ::kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);
#else
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        ::epoll_ctl(event_fd_, EPOLL_CTL_ADD, fd, &ev);
#endif
    }
}

void WorkerThread::run() {
#ifdef STRIKE_PLATFORM_MACOS
    struct kevent events[kMaxEvents];
    while (running_.load(std::memory_order_acquire)) {
        struct timespec timeout{0, 100000000}; // 100ms
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
        int n = ::epoll_wait(event_fd_, events, kMaxEvents, 100);
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

    uint8_t buf[kReadChunk];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            conn.read_buf.insert(conn.read_buf.end(), buf, buf + n);
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
        uint32_t raw;
        std::memcpy(&raw, conn.read_buf.data(), 4);
        uint32_t frame_size = strike::bswap32(raw);

        if (frame_size > kMaxFrameSize) {
            std::cerr << "  Frame too large (" << frame_size << " bytes), closing connection\n";
            close_connection(conn.fd);
            return;
        }

        size_t total = 4 + frame_size;
        if (conn.read_buf.size() < total) break;

        uint8_t resp_buf[65536];
        size_t resp_len = router_.route_request(
            conn.read_buf.data() + 4, frame_size, resp_buf, sizeof(resp_buf));

        if (resp_len > 0) {
            conn.queue_write(resp_buf, resp_len);
            enable_write(conn.fd);
        }

        conn.read_buf.erase(conn.read_buf.begin(),
                            conn.read_buf.begin() + static_cast<ptrdiff_t>(total));
    }
}

void WorkerThread::handle_write(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    Connection& conn = it->second;

    while (conn.has_pending_write()) {
        size_t remaining = conn.write_buf.size() - conn.write_offset;
        ssize_t n = ::send(fd, conn.write_buf.data() + conn.write_offset, remaining, 0);
        if (n > 0) {
            conn.write_offset += static_cast<size_t>(n);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_connection(fd);
            return;
        }
    }

    if (!conn.has_pending_write()) {
        conn.write_buf.clear();
        conn.write_offset = 0;
        disable_write(fd);
    } else {
        conn.compact_write_buf();
    }
}

void WorkerThread::close_connection(int fd) {
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
    std::cout << "  - Connection closed (fd=" << fd << ", worker=" << id_ << ")\n" << std::flush;
}

void WorkerThread::enable_write(int fd) {
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

void WorkerThread::disable_write(int fd) {
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
#else
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
#ifdef STRIKE_PLATFORM_MACOS
    struct kevent events[kMaxEvents];
    while (running_.load(std::memory_order_acquire)) {
        struct timespec timeout{0, 100000000}; // 100ms
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
        int n = ::epoll_wait(event_fd_, events, kMaxEvents, 100);
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
