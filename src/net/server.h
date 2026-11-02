#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "engine/database.h"
#include "net/protocol.h"
#include "net/socket.h"

namespace atlas {

struct ServerOptions {
    std::string host = "127.0.0.1";
    uint16_t port = proto::DEFAULT_PORT;  // 0 = pick a free port
    size_t max_connections = 64;
    std::string name = "AtlasDB 0.1.0";
    // A client that leaves a transaction open without sending anything for this
    // long has it rolled back and its connection closed, so it cannot keep the
    // database locked forever. Zero disables the timeout.
    std::chrono::milliseconds idle_transaction_timeout{60'000};
};

// TCP server speaking the AtlasDB wire protocol. One thread per connection,
// each with its own Session.
class Server {
public:
    Server(Database& db, ServerOptions options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
    void stop();
    uint16_t port() const { return listener_.port(); }
    size_t active_connections() const;

private:
    struct Connection {
        net::Socket socket;
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    void accept_loop();
    void serve(Connection& conn);
    void reap_finished();

    Database& db_;
    ServerOptions options_;
    net::Listener listener_;
    std::thread accept_thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::list<std::unique_ptr<Connection>> connections_;
};

}  // namespace atlas
