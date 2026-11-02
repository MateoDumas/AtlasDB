#include "net/client.h"

#include "common/error.h"

namespace atlas {

namespace {

proto::Message expect_message(net::Socket& s) {
    auto m = proto::read_message(s);
    if (!m) throw DbError(ErrorCode::IO, "server closed the connection");
    return std::move(*m);
}

QueryResult last_or_empty(std::vector<QueryResult>& results) {
    if (results.empty()) return QueryResult{};
    return std::move(results.back());
}

}  // namespace

Client Client::connect(const std::string& host, uint16_t port, const std::string& client_name) {
    Client c;
    c.sock_ = net::Socket::connect(host, port);
    proto::write_message(c.sock_, proto::MsgType::Hello, proto::encode_hello(client_name));
    proto::Message m = expect_message(c.sock_);
    if (m.type == proto::MsgType::Error) throw proto::decode_error(m.body);
    if (m.type != proto::MsgType::HelloOk) throw DbError(ErrorCode::Protocol, "expected HelloOk from server");
    c.server_name_ = proto::decode_hello_ok(m.body);
    m = expect_message(c.sock_);
    if (m.type != proto::MsgType::Ready) throw DbError(ErrorCode::Protocol, "expected Ready from server");
    c.in_txn_ = proto::decode_ready(m.body);
    return c;
}

Client::~Client() {
    try {
        close();
    } catch (...) {
    }
}

void Client::close() {
    if (!sock_.valid()) return;
    try {
        proto::write_message(sock_, proto::MsgType::Terminate, {});
    } catch (...) {
    }
    sock_.close();
}

void Client::send(proto::MsgType type, const std::string& body) {
    if (!sock_.valid()) throw DbError(ErrorCode::IO, "client is not connected");
    // Anything waiting before we send is unsolicited: the server terminated the
    // connection (e.g. idle-transaction timeout). Report its reason.
    if (sock_.wait_readable(0)) {
        auto m = proto::read_message(sock_);
        sock_.close();
        in_txn_ = false;
        if (m && m->type == proto::MsgType::Error) throw proto::decode_error(m->body);
        throw DbError(ErrorCode::IO, "server closed the connection");
    }
    proto::write_message(sock_, type, body);
}

std::optional<DbError> Client::read_until_ready(const std::function<void(QueryResult&&)>& on_result,
                                                size_t* prepared_params) {
    std::optional<DbError> error;
    while (true) {
        auto next = proto::read_message(sock_);
        if (!next) {
            sock_.close();
            in_txn_ = false;
            if (error) throw *error;
            throw DbError(ErrorCode::IO, "server closed the connection");
        }
        proto::Message m = std::move(*next);
        switch (m.type) {
            case proto::MsgType::RowSet: on_result(proto::decode_rowset(m.body)); break;
            case proto::MsgType::Complete: on_result(proto::decode_complete(m.body)); break;
            case proto::MsgType::Prepared:
                if (prepared_params) *prepared_params = proto::decode_prepared(m.body);
                break;
            case proto::MsgType::Error: error = proto::decode_error(m.body); break;
            case proto::MsgType::Ready: in_txn_ = proto::decode_ready(m.body); return error;
            default: throw DbError(ErrorCode::Protocol, "unexpected message from server");
        }
    }
}

std::vector<QueryResult> Client::query(std::string_view sql) {
    std::vector<QueryResult> results;
    query_stream(sql, [&](QueryResult&& r) { results.push_back(std::move(r)); });
    return results;
}

void Client::query_stream(std::string_view sql, const std::function<void(QueryResult&&)>& on_result) {
    send(proto::MsgType::Query, proto::encode_query(sql));
    if (auto error = read_until_ready(on_result)) throw *error;
}

QueryResult Client::query_one(std::string_view sql) {
    auto results = query(sql);
    return last_or_empty(results);
}

QueryResult Client::query(std::string_view sql, const std::vector<Value>& params) {
    // Prepare under the unnamed statement and execute it in one round trip:
    // both requests are sent before reading either reply.
    send(proto::MsgType::Prepare, proto::encode_prepare("", sql));
    proto::write_message(sock_, proto::MsgType::Execute, proto::encode_execute("", params));
    std::vector<QueryResult> results;
    auto collect = [&](QueryResult&& r) { results.push_back(std::move(r)); };
    std::optional<DbError> prepare_error = read_until_ready(collect);
    std::optional<DbError> execute_error = read_until_ready(collect);
    // If preparing failed, the execute error ("does not exist") is only a consequence.
    if (prepare_error) throw *prepare_error;
    if (execute_error) throw *execute_error;
    return last_or_empty(results);
}

PreparedStatement Client::prepare(std::string_view sql) {
    std::string name = "s" + std::to_string(next_statement_++);
    send(proto::MsgType::Prepare, proto::encode_prepare(name, sql));
    size_t params = 0;
    if (auto error = read_until_ready([](QueryResult&&) {}, &params)) throw *error;
    return PreparedStatement(this, std::move(name), params);
}

QueryResult Client::execute_prepared(const std::string& name, const std::vector<Value>& params) {
    send(proto::MsgType::Execute, proto::encode_execute(name, params));
    std::vector<QueryResult> results;
    if (auto error = read_until_ready([&](QueryResult&& r) { results.push_back(std::move(r)); })) throw *error;
    return last_or_empty(results);
}

void Client::close_prepared(const std::string& name) {
    send(proto::MsgType::Close, proto::encode_close(name));
    if (auto error = read_until_ready([](QueryResult&&) {})) throw *error;
}

QueryResult PreparedStatement::execute(const std::vector<Value>& params) {
    return client_->execute_prepared(name_, params);
}

void PreparedStatement::close() { client_->close_prepared(name_); }

}  // namespace atlas
