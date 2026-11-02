#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace atlas::net {

#ifdef _WIN32
using socket_t = uintptr_t;
#else
using socket_t = int;
#endif

// Initializes the platform socket library (WSAStartup on Windows). Idempotent.
void init();

// Blocking TCP stream socket.
class Socket {
public:
    Socket() = default;
    explicit Socket(socket_t fd) : fd_(fd), valid_(true) {}
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept;
    Socket& operator=(Socket&& o) noexcept;

    static Socket connect(const std::string& host, uint16_t port);

    void send_all(const void* data, size_t n);
    // Reads exactly n bytes; returns false on orderly EOF before any byte was read.
    bool recv_exact(void* data, size_t n);
    // Waits up to timeout_ms for incoming data (or EOF). Returns true if a read would not block.
    bool wait_readable(int timeout_ms);
    void shutdown();
    void close();
    bool valid() const { return valid_; }

private:
    socket_t fd_ = 0;
    bool valid_ = false;
};

class Listener {
public:
    Listener() = default;
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // Binds and listens. Port 0 picks an ephemeral port (see port()).
    void listen(const std::string& host, uint16_t port, int backlog = 64);
    // Blocks until a client connects; returns an invalid socket once closed.
    Socket accept();
    uint16_t port() const { return port_; }
    void close();

private:
    socket_t fd_ = 0;
    bool valid_ = false;
    uint16_t port_ = 0;
};

}  // namespace atlas::net
