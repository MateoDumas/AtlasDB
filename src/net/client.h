#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"
#include "net/protocol.h"
#include "net/socket.h"

namespace atlas {

class Client;

// A statement prepared on the server: parsed once, executed many times.
// It refers to its Client, which must stay alive (and must not be moved) while
// the handle is used.
class PreparedStatement {
public:
    QueryResult execute(const std::vector<Value>& params = {});
    size_t param_count() const { return param_count_; }
    const std::string& name() const { return name_; }
    // Releases the statement on the server.
    void close();

private:
    friend class Client;
    PreparedStatement(Client* client, std::string name, size_t params)
        : client_(client), name_(std::move(name)), param_count_(params) {}
    Client* client_;
    std::string name_;
    size_t param_count_;
};

// Client library for the AtlasDB wire protocol.
//
//   auto c = atlas::Client::connect("127.0.0.1", 5480);
//   c.query("INSERT INTO t VALUES ($1, $2)", atlas::params(1, "x"));
//   auto rows = c.query_one("SELECT * FROM t").rows;
//
//   auto find = c.prepare("SELECT name FROM users WHERE id = ?");
//   for (int id : ids) find.execute(atlas::params(id));
class Client {
public:
    static Client connect(const std::string& host, uint16_t port = proto::DEFAULT_PORT,
                          const std::string& client_name = "atlas-client");

    Client(Client&&) noexcept = default;
    Client& operator=(Client&&) noexcept = default;
    ~Client();

    // Runs one or more statements. Throws DbError (with the server's error
    // code) if any statement fails; statements before it have still executed.
    std::vector<QueryResult> query(std::string_view sql);
    // Like query(), but hands each statement's result to `on_result` as it arrives.
    void query_stream(std::string_view sql, const std::function<void(QueryResult&&)>& on_result);
    // Convenience: runs the SQL and returns the result of the last statement.
    QueryResult query_one(std::string_view sql);

    // Runs one statement with $n / ? placeholders bound to `params`. The values
    // are sent separately from the SQL text, so they cannot inject SQL.
    QueryResult query(std::string_view sql, const std::vector<Value>& params);

    // Prepares a statement under a generated name.
    PreparedStatement prepare(std::string_view sql);

    bool in_transaction() const { return in_txn_; }
    const std::string& server_name() const { return server_name_; }
    bool connected() const { return sock_.valid(); }
    void close();

private:
    friend class PreparedStatement;
    Client() = default;

    void send(proto::MsgType type, const std::string& body);
    // Reads replies up to and including Ready. Returns the server's error, if any.
    std::optional<DbError> read_until_ready(const std::function<void(QueryResult&&)>& on_result,
                                            size_t* prepared_params = nullptr);
    QueryResult execute_prepared(const std::string& name, const std::vector<Value>& params);
    void close_prepared(const std::string& name);

    net::Socket sock_;
    std::string server_name_;
    bool in_txn_ = false;
    uint64_t next_statement_ = 1;
};

}  // namespace atlas
