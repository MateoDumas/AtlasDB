#include "net/server.h"

#include "engine/session.h"
#include "sql/parser.h"

namespace atlas {

Server::Server(Database& db, ServerOptions options) : db_(db), options_(std::move(options)) {}

Server::~Server() { stop(); }

void Server::start() {
    listener_.listen(options_.host, options_.port);
    running_ = true;
    accept_thread_ = std::thread([this] { accept_loop(); });
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    listener_.close();
    if (accept_thread_.joinable()) accept_thread_.join();
    std::list<std::unique_ptr<Connection>> conns;
    {
        std::lock_guard lk(mutex_);
        for (auto& c : connections_) {
            std::lock_guard close_lk(c->close_mutex);
            c->socket.shutdown();
        }
        conns.swap(connections_);
    }
    for (auto& c : conns)
        if (c->thread.joinable()) c->thread.join();
}

size_t Server::active_connections() const {
    std::lock_guard lk(mutex_);
    size_t n = 0;
    for (const auto& c : connections_) n += c->finished ? 0 : 1;
    return n;
}

void Server::reap_finished() {
    std::list<std::unique_ptr<Connection>> done;
    {
        std::lock_guard lk(mutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            if ((*it)->finished) {
                done.push_back(std::move(*it));
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& c : done)
        if (c->thread.joinable()) c->thread.join();
}

void Server::accept_loop() {
    while (running_) {
        net::Socket sock = listener_.accept();
        if (!sock.valid()) {
            if (!running_) break;
            continue;
        }
        reap_finished();
        if (active_connections() >= options_.max_connections) {
            try {
                proto::write_message(sock, proto::MsgType::Error,
                                     proto::encode_error(ErrorCode::Locked, "too many connections"));
            } catch (...) {
            }
            continue;
        }
        auto conn = std::make_unique<Connection>();
        conn->socket = std::move(sock);
        Connection* raw = conn.get();
        std::lock_guard lk(mutex_);
        connections_.push_back(std::move(conn));
        raw->thread = std::thread([this, raw] {
            serve(*raw);
            raw->finished = true;
        });
    }
}

void Server::serve(Connection& conn) {
    net::Socket& sock = conn.socket;
    auto send_error = [&](ErrorCode code, const std::string& msg) {
        proto::write_message(sock, proto::MsgType::Error, proto::encode_error(code, msg));
    };
    try {
        auto hello = proto::read_message(sock);
        if (!hello) return;
        if (hello->type != proto::MsgType::Hello) {
            send_error(ErrorCode::Protocol, "expected Hello message");
            return;
        }
        try {
            proto::decode_hello(hello->body);
        } catch (const DbError& e) {
            send_error(e.code(), e.what());
            return;
        }
        proto::write_message(sock, proto::MsgType::HelloOk, proto::encode_hello_ok(options_.name));

        Session session(db_);
        proto::write_message(sock, proto::MsgType::Ready, proto::encode_ready(false));

        while (true) {
            // Poll so that stop() and the idle-transaction timeout are noticed
            // even while the client sends nothing.
            auto idle_since = std::chrono::steady_clock::now();
            bool timed_out = false;
            while (running_ && !sock.wait_readable(100)) {
                auto timeout = options_.idle_transaction_timeout;
                if (timeout.count() > 0 && session.in_transaction() &&
                    std::chrono::steady_clock::now() - idle_since >= timeout) {
                    timed_out = true;
                    break;
                }
            }
            if (timed_out) {
                session.abort_transaction();
                send_error(ErrorCode::Transaction,
                           "terminating connection: transaction was idle for more than " +
                               std::to_string(options_.idle_transaction_timeout.count()) +
                               " ms and has been rolled back");
                break;
            }
            if (!running_) break;
            auto msg = proto::read_message(sock);
            if (!msg || msg->type == proto::MsgType::Terminate) break;
            auto send_result = [&](const QueryResult& r) {
                auto [type, body] = proto::encode_result(r);
                proto::write_message(sock, type, body);
            };
            try {
                switch (msg->type) {
                    case proto::MsgType::Query: {
                        std::string sql = proto::decode_query(msg->body);
                        std::vector<Statement> stmts = parse_sql(sql);
                        for (auto& stmt : stmts) send_result(session.execute_statement(stmt));
                        break;
                    }
                    case proto::MsgType::Prepare: {
                        auto [name, sql] = proto::decode_prepare(msg->body);
                        size_t n = session.prepare(name, sql);
                        proto::write_message(sock, proto::MsgType::Prepared, proto::encode_prepared(n));
                        break;
                    }
                    case proto::MsgType::Execute: {
                        auto [name, params] = proto::decode_execute(msg->body);
                        send_result(session.execute_prepared(name, params));
                        break;
                    }
                    case proto::MsgType::Close: {
                        session.deallocate(proto::decode_close(msg->body));
                        QueryResult r;
                        r.tag = "DEALLOCATE";
                        send_result(r);
                        break;
                    }
                    default: throw DbError(ErrorCode::Protocol, "unexpected message type");
                }
            } catch (const DbError& e) {
                if (e.code() == ErrorCode::IO && !running_) break;
                send_error(e.code(), e.what());
            } catch (const std::exception& e) {
                send_error(ErrorCode::Internal, e.what());
            }
            proto::write_message(sock, proto::MsgType::Ready, proto::encode_ready(session.in_transaction()));
        }
    } catch (...) {
        // Connection-level failure (peer vanished, protocol violation): just drop it.
    }
    std::lock_guard close_lk(conn.close_mutex);
    sock.close();
}

}  // namespace atlas
