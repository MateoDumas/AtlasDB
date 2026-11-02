#include "net/socket.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "common/error.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace atlas::net {

namespace {

#ifdef _WIN32
constexpr socket_t INVALID = static_cast<socket_t>(INVALID_SOCKET);
int last_error() { return WSAGetLastError(); }
void close_fd(socket_t fd) { ::closesocket(static_cast<SOCKET>(fd)); }
#else
constexpr socket_t INVALID = -1;
int last_error() { return errno; }
void close_fd(socket_t fd) { ::close(fd); }
#endif

[[noreturn]] void fail(const std::string& what) {
    throw DbError(ErrorCode::IO, what + " (socket error " + std::to_string(last_error()) + ")");
}

void set_nodelay(socket_t fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}

addrinfo* resolve(const std::string& host, uint16_t port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (passive) hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    std::string service = std::to_string(port);
    int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0 || !res) throw DbError(ErrorCode::IO, "cannot resolve host '" + host + "'");
    return res;
}

}  // namespace

void init() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw DbError(ErrorCode::IO, "WSAStartup failed");
    });
#endif
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& o) noexcept : fd_(o.fd_), valid_(o.valid_) { o.valid_ = false; }

Socket& Socket::operator=(Socket&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        valid_ = o.valid_;
        o.valid_ = false;
    }
    return *this;
}

Socket Socket::connect(const std::string& host, uint16_t port) {
    init();
    addrinfo* res = resolve(host, port, false);
    socket_t fd = INVALID;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = static_cast<socket_t>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (fd == INVALID) continue;
        if (::connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
        close_fd(fd);
        fd = INVALID;
    }
    ::freeaddrinfo(res);
    if (fd == INVALID) fail("cannot connect to " + host + ":" + std::to_string(port));
    set_nodelay(fd);
    return Socket(fd);
}

void Socket::send_all(const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n > 0) {
        int chunk = static_cast<int>(std::min<size_t>(n, 1 << 20));
#ifdef _WIN32
        int w = ::send(static_cast<SOCKET>(fd_), p, chunk, 0);
#else
        ssize_t w = ::send(fd_, p, static_cast<size_t>(chunk), MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
#endif
        if (w <= 0) fail("send failed");
        p += w;
        n -= static_cast<size_t>(w);
    }
}

bool Socket::recv_exact(void* data, size_t n) {
    char* p = static_cast<char*>(data);
    size_t got = 0;
    while (got < n) {
        int chunk = static_cast<int>(std::min<size_t>(n - got, 1 << 20));
#ifdef _WIN32
        int r = ::recv(static_cast<SOCKET>(fd_), p + got, chunk, 0);
#else
        ssize_t r = ::recv(fd_, p + got, static_cast<size_t>(chunk), 0);
        if (r < 0 && errno == EINTR) continue;
#endif
        if (r == 0) {
            if (got == 0) return false;
            throw DbError(ErrorCode::Protocol, "connection closed in the middle of a message");
        }
        if (r < 0) fail("recv failed");
        got += static_cast<size_t>(r);
    }
    return true;
}

void Socket::shutdown() {
    if (!valid_) return;
#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(fd_), SD_BOTH);
#else
    ::shutdown(fd_, SHUT_RDWR);
#endif
}

void Socket::close() {
    if (valid_) {
        close_fd(fd_);
        valid_ = false;
    }
}

bool Socket::wait_readable(int timeout_ms) {
    if (!valid_) return true;
    fd_set set;
    FD_ZERO(&set);
#ifdef _WIN32
    FD_SET(static_cast<SOCKET>(fd_), &set);
#else
    FD_SET(fd_, &set);
#endif
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = ::select(static_cast<int>(fd_) + 1, &set, nullptr, nullptr, &tv);
    return rc != 0;  // errors count as "readable" so the caller's recv reports them
}

Listener::~Listener() { close(); }

void Listener::listen(const std::string& host, uint16_t port, int backlog) {
    init();
    addrinfo* res = resolve(host, port, true);
    socket_t fd = static_cast<socket_t>(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (fd == INVALID) {
        ::freeaddrinfo(res);
        fail("cannot create socket");
    }
    int one = 1;
#ifndef _WIN32
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#else
    (void)one;
#endif
    if (::bind(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        ::freeaddrinfo(res);
        close_fd(fd);
        fail("cannot bind " + host + ":" + std::to_string(port));
    }
    ::freeaddrinfo(res);
    if (::listen(fd, backlog) != 0) {
        close_fd(fd);
        fail("listen failed");
    }
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    if (addr.ss_family == AF_INET) port_ = ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
    else port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
    fd_ = fd;
    valid_ = true;
}

Socket Listener::accept() {
    if (!valid_) return Socket();
    socket_t c = static_cast<socket_t>(::accept(fd_, nullptr, nullptr));
    if (c == INVALID) return Socket();
    set_nodelay(c);
    return Socket(c);
}

void Listener::close() {
    if (valid_) {
        valid_ = false;
#ifdef _WIN32
        ::closesocket(static_cast<SOCKET>(fd_));
#else
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
#endif
    }
}

}  // namespace atlas::net
