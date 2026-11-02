#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/result.h"
#include "engine/database.h"
#include "sql/ast.h"

namespace atlas {

// A connection-like handle for executing SQL against a Database.
//
// Each session runs either in autocommit mode (every statement is its own
// transaction) or inside an explicit BEGIN ... COMMIT/ROLLBACK block. A
// failing statement inside an explicit transaction is rolled back on its own;
// the transaction stays open. Destroying a session rolls back any open
// transaction.
class Session {
public:
    explicit Session(Database& db);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Executes every statement of a script; throws DbError at the first failure.
    std::vector<QueryResult> execute(std::string_view sql);
    // Executes a single statement whose $n / ? placeholders take `params`.
    // Values never pass through the SQL parser, so they cannot inject SQL.
    QueryResult execute(std::string_view sql, const std::vector<Value>& params);
    QueryResult execute_statement(Statement& stmt);

    // Prepared statements: parsed once, executed many times with different
    // parameters. Names are per session; preparing an existing name replaces it.
    // Returns the number of parameters the statement expects.
    size_t prepare(const std::string& name, std::string_view sql);
    QueryResult execute_prepared(const std::string& name, const std::vector<Value>& params);
    void deallocate(const std::string& name);
    bool has_prepared(const std::string& name) const { return prepared_.count(name) > 0; }

    bool in_transaction() const { return txn_ != nullptr; }
    // Rolls back an open explicit transaction and releases the lock.
    // Returns false if there was none.
    bool abort_transaction();
    uint64_t id() const { return id_; }

private:
    QueryResult run_select(SelectStmt& s);
    QueryResult run_insert(Transaction& txn, InsertStmt& s);
    QueryResult run_update(Transaction& txn, UpdateStmt& s);
    QueryResult run_delete(Transaction& txn, DeleteStmt& s);
    QueryResult run_explain(ExplainStmt& s);
    QueryResult run_show_tables();
    QueryResult run_describe(const std::string& table);
    void acquire_lock();
    void release_lock();
    void acquire_shared_lock();

    struct Prepared {
        Statement stmt;  // pristine copy; cloned for every execution
        size_t params;
    };

    Database& db_;
    uint64_t id_;
    std::unique_ptr<Transaction> txn_;
    std::unordered_map<std::string, Prepared> prepared_;
};

}  // namespace atlas
