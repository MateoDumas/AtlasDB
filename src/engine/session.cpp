#include "engine/session.h"

#include <atomic>
#include <set>

#include "common/error.h"
#include "engine/planner.h"
#include "sql/parser.h"

namespace atlas {

namespace {

std::atomic<uint64_t> next_session_id{1};

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

QueryResult command(std::string tag, uint64_t affected = 0) {
    QueryResult r;
    r.tag = std::move(tag);
    r.affected = affected;
    return r;
}

bool is_ddl(const Statement& s) {
    return std::holds_alternative<CreateTableStmt>(s.node) || std::holds_alternative<DropTableStmt>(s.node) ||
           std::holds_alternative<CreateIndexStmt>(s.node) || std::holds_alternative<DropIndexStmt>(s.node) ||
           std::holds_alternative<CheckpointStmt>(s.node);
}

bool is_dml(const Statement& s) {
    return std::holds_alternative<InsertStmt>(s.node) || std::holds_alternative<UpdateStmt>(s.node) ||
           std::holds_alternative<DeleteStmt>(s.node);
}

// Statements that never modify data; in autocommit mode they share the lock.
bool is_read_only(const Statement& s) {
    return std::holds_alternative<SelectStmt>(s.node) || std::holds_alternative<ExplainStmt>(s.node) ||
           std::holds_alternative<ShowTablesStmt>(s.node) || std::holds_alternative<DescribeStmt>(s.node);
}

}  // namespace

Session::Session(Database& db) : db_(db), id_(next_session_id++) {}

Session::~Session() { abort_transaction(); }

bool Session::abort_transaction() {
    if (!txn_) return false;
    try {
        db_.rollback(*txn_);
    } catch (...) {
    }
    txn_.reset();
    release_lock();
    return true;
}

void Session::acquire_lock() {
    if (!db_.lock().acquire(id_, db_.options().lock_timeout))
        throw DbError(ErrorCode::Locked, "database is locked by another transaction (timed out)");
}

void Session::release_lock() { db_.lock().release(id_); }

void Session::acquire_shared_lock() {
    if (!db_.lock().acquire_shared(db_.options().lock_timeout))
        throw DbError(ErrorCode::Locked, "database is locked by another transaction (timed out)");
}

std::vector<QueryResult> Session::execute(std::string_view sql) {
    std::vector<Statement> stmts = parse_sql(sql);
    std::vector<QueryResult> out;
    out.reserve(stmts.size());
    for (auto& s : stmts) out.push_back(execute_statement(s));
    return out;
}

QueryResult Session::execute(std::string_view sql, const std::vector<Value>& params) {
    Statement stmt = parse_statement(sql);
    bind_parameters(stmt, params);
    return execute_statement(stmt);
}

size_t Session::prepare(const std::string& name, std::string_view sql) {
    // Forget the old definition first: if parsing fails, executing `name` must
    // not silently run the previous statement.
    prepared_.erase(name);
    Statement stmt = parse_statement(sql);
    size_t n = count_parameters(stmt);
    prepared_.insert_or_assign(name, Prepared{std::move(stmt), n});
    return n;
}

QueryResult Session::execute_prepared(const std::string& name, const std::vector<Value>& params) {
    auto it = prepared_.find(name);
    if (it == prepared_.end())
        throw DbError(ErrorCode::Semantic, "prepared statement '" + name + "' does not exist");
    Statement stmt = clone_statement(it->second.stmt);
    bind_parameters(stmt, params);
    return execute_statement(stmt);
}

void Session::deallocate(const std::string& name) {
    if (!prepared_.erase(name))
        throw DbError(ErrorCode::Semantic, "prepared statement '" + name + "' does not exist");
}

QueryResult Session::execute_statement(Statement& stmt) {
    if (size_t n = count_parameters(stmt))
        throw DbError(ErrorCode::Semantic, "statement has " + std::to_string(n) +
                                               " parameter placeholder(s) but no values were supplied");
    // ---- transaction control ----
    if (std::holds_alternative<BeginStmt>(stmt.node)) {
        if (txn_) throw DbError(ErrorCode::Transaction, "a transaction is already in progress");
        acquire_lock();
        txn_ = db_.begin(true);
        return command("BEGIN");
    }
    if (std::holds_alternative<CommitStmt>(stmt.node)) {
        if (!txn_) throw DbError(ErrorCode::Transaction, "no transaction in progress");
        std::unique_ptr<Transaction> txn = std::move(txn_);
        try {
            db_.commit(*txn);
        } catch (...) {
            release_lock();
            throw;
        }
        release_lock();
        return command("COMMIT");
    }
    if (std::holds_alternative<RollbackStmt>(stmt.node)) {
        if (!txn_) throw DbError(ErrorCode::Transaction, "no transaction in progress");
        std::unique_ptr<Transaction> txn = std::move(txn_);
        try {
            db_.rollback(*txn);
        } catch (...) {
            release_lock();
            throw;
        }
        release_lock();
        return command("ROLLBACK");
    }

    // ---- DDL: always runs as its own transaction ----
    if (is_ddl(stmt)) {
        if (txn_)
            throw DbError(ErrorCode::Transaction, "schema changes and CHECKPOINT are not allowed inside a transaction");
        acquire_lock();
        struct Release {
            Session* s;
            ~Release() { s->release_lock(); }
        } guard{this};
        return std::visit(
            Overloaded{
                [&](CreateTableStmt& s) {
                    db_.create_table(s);
                    return command("CREATE TABLE");
                },
                [&](DropTableStmt& s) {
                    db_.drop_table(s.name, s.if_exists);
                    return command("DROP TABLE");
                },
                [&](CreateIndexStmt& s) {
                    db_.create_index(s);
                    return command("CREATE INDEX");
                },
                [&](DropIndexStmt& s) {
                    db_.drop_index(s.name, s.if_exists);
                    return command("DROP INDEX");
                },
                [&](CheckpointStmt&) {
                    db_.checkpoint();
                    return command("CHECKPOINT");
                },
                [&](auto&) -> QueryResult { throw DbError(ErrorCode::Internal, "unexpected statement"); },
            },
            stmt.node);
    }

    // ---- reads and writes: autocommit unless inside BEGIN ----
    bool autocommit = !txn_;
    bool shared = autocommit && is_read_only(stmt);
    auto release = [&] {
        if (shared) db_.lock().release_shared();
        else if (autocommit) release_lock();
    };
    if (shared) acquire_shared_lock();
    else if (autocommit) acquire_lock();
    std::unique_ptr<Transaction> local;
    Transaction* txn = txn_.get();
    if (autocommit) {
        local = db_.begin(false);
        txn = local.get();
    }
    Transaction::Savepoint sp = txn->savepoint();
    try {
        QueryResult result = std::visit(
            Overloaded{
                [&](SelectStmt& s) { return run_select(s); },
                [&](InsertStmt& s) { return run_insert(*txn, s); },
                [&](UpdateStmt& s) { return run_update(*txn, s); },
                [&](DeleteStmt& s) { return run_delete(*txn, s); },
                [&](ExplainStmt& s) { return run_explain(s); },
                [&](ShowTablesStmt&) { return run_show_tables(); },
                [&](DescribeStmt& s) { return run_describe(s.table); },
                [&](auto&) -> QueryResult { throw DbError(ErrorCode::Internal, "unexpected statement"); },
            },
            stmt.node);
        if (autocommit && is_dml(stmt)) db_.commit(*txn);
        release();
        return result;
    } catch (...) {
        try {
            db_.rollback(*txn, sp);
        } catch (...) {
        }
        release();
        throw;
    }
}

// ---------------------------------------------------------------------------

QueryResult Session::run_select(SelectStmt& s) {
    SelectPlan plan = plan_select(db_, s);
    QueryResult r;
    r.has_rows = true;
    r.columns = std::move(plan.column_names);
    r.types = std::move(plan.column_types);
    plan.root->open();
    Row row;
    while (plan.root->next(row)) r.rows.push_back(row);
    r.tag = "SELECT " + std::to_string(r.rows.size());
    r.affected = r.rows.size();
    return r;
}

QueryResult Session::run_insert(Transaction& txn, InsertStmt& s) {
    Table& t = db_.table(s.table);
    const auto& cols = t.schema().columns;
    std::vector<int> targets;
    if (s.columns.empty()) {
        for (size_t i = 0; i < cols.size(); ++i) targets.push_back(static_cast<int>(i));
    } else {
        std::set<int> seen;
        for (const auto& name : s.columns) {
            int idx = t.schema().column_index(name);
            if (idx < 0) throw DbError(ErrorCode::Semantic, "table '" + t.name() + "' has no column '" + name + "'");
            if (!seen.insert(idx).second) throw DbError(ErrorCode::Semantic, "column '" + name + "' specified twice");
            targets.push_back(idx);
        }
    }

    std::vector<Row> rows;
    if (s.select) {
        QueryResult sel = run_select(*s.select);  // fully materialized before inserting
        if (sel.columns.size() != targets.size())
            throw DbError(ErrorCode::Semantic, "INSERT has " + std::to_string(targets.size()) +
                                                   " target columns but SELECT returns " +
                                                   std::to_string(sel.columns.size()));
        rows = std::move(sel.rows);
    } else {
        Scope empty;
        for (auto& exprs : s.rows) {
            if (exprs.size() != targets.size())
                throw DbError(ErrorCode::Semantic, "INSERT has " + std::to_string(targets.size()) +
                                                       " target columns but " + std::to_string(exprs.size()) +
                                                       " values");
            Row values;
            for (auto& e : exprs) {
                if (contains_aggregate(*e)) throw DbError(ErrorCode::Semantic, "aggregates are not allowed in VALUES");
                bind_expr(*e, empty);
                values.push_back(eval_expr(*e, {}));
            }
            rows.push_back(std::move(values));
        }
    }

    for (auto& values : rows) db_.insert_row(txn, t, db_.complete_row(t, targets, std::move(values)));
    return command("INSERT " + std::to_string(rows.size()), rows.size());
}

QueryResult Session::run_update(Transaction& txn, UpdateStmt& s) {
    Table& t = db_.table(s.table);
    Scope scope;
    scope.add_table(t.name(), t.schema());
    std::vector<std::pair<size_t, ExprPtr>> sets;
    std::set<int> seen;
    for (auto& [name, expr] : s.assignments) {
        int idx = t.schema().column_index(name);
        if (idx < 0) throw DbError(ErrorCode::Semantic, "table '" + t.name() + "' has no column '" + name + "'");
        if (!seen.insert(idx).second) throw DbError(ErrorCode::Semantic, "column '" + name + "' assigned twice");
        if (contains_aggregate(*expr)) throw DbError(ErrorCode::Semantic, "aggregates are not allowed in UPDATE");
        bind_expr(*expr, scope);
        sets.emplace_back(static_cast<size_t>(idx), std::move(expr));
    }
    TableScanPlan plan = plan_table_scan(t, std::move(s.where));
    auto matches = plan.collect();
    for (auto& [rid, old_row] : matches) {
        Row new_row = old_row;
        for (auto& [col, expr] : sets) new_row[col] = eval_expr(*expr, old_row);
        db_.update_row(txn, t, rid, old_row, std::move(new_row));
    }
    return command("UPDATE " + std::to_string(matches.size()), matches.size());
}

QueryResult Session::run_delete(Transaction& txn, DeleteStmt& s) {
    Table& t = db_.table(s.table);
    TableScanPlan plan = plan_table_scan(t, std::move(s.where));
    auto matches = plan.collect();
    for (auto& [rid, row] : matches) db_.delete_row(txn, t, rid, row);
    return command("DELETE " + std::to_string(matches.size()), matches.size());
}

QueryResult Session::run_explain(ExplainStmt& s) {
    std::string text;
    Statement& inner = *s.inner;
    if (auto* sel = std::get_if<SelectStmt>(&inner.node)) {
        text = plan_select(db_, *sel).root->explain();
    } else if (auto* upd = std::get_if<UpdateStmt>(&inner.node)) {
        text = plan_table_scan(db_.table(upd->table), std::move(upd->where)).explain("Update");
    } else if (auto* del = std::get_if<DeleteStmt>(&inner.node)) {
        text = plan_table_scan(db_.table(del->table), std::move(del->where)).explain("Delete");
    } else {
        throw DbError(ErrorCode::Semantic, "EXPLAIN supports SELECT, UPDATE and DELETE");
    }
    QueryResult r;
    r.has_rows = true;
    r.columns = {"QUERY PLAN"};
    r.types = {DataType::Text};
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        r.rows.push_back({Value::text(text.substr(start, end - start))});
        start = end + 1;
    }
    r.tag = "EXPLAIN";
    return r;
}

QueryResult Session::run_show_tables() {
    QueryResult r;
    r.has_rows = true;
    r.columns = {"table", "columns", "rows", "indexes"};
    r.types = {DataType::Text, DataType::Integer, DataType::Integer, DataType::Text};
    for (Table* t : db_.tables()) {
        std::string idx;
        for (const auto& i : t->indexes()) idx += (idx.empty() ? "" : ", ") + i->def().name;
        r.rows.push_back({Value::text(t->name()), Value::integer(static_cast<int64_t>(t->schema().columns.size())),
                          Value::integer(static_cast<int64_t>(t->row_count())), Value::text(idx)});
    }
    r.tag = "SHOW TABLES";
    return r;
}

QueryResult Session::run_describe(const std::string& table) {
    Table& t = db_.table(table);
    QueryResult r;
    r.has_rows = true;
    r.columns = {"column", "type", "nullable", "key", "default", "references"};
    r.types.assign(r.columns.size(), DataType::Text);
    for (const auto& c : t.schema().columns) {
        std::string key = c.primary_key ? "PRIMARY" : (c.unique ? "UNIQUE" : "");
        if (key.empty() && t.index_on(t.schema().column_index(c.name))) key = "INDEX";
        Value def = c.default_sql.empty() ? Value::null() : Value::text(c.default_sql);
        Value ref = c.foreign_key ? Value::text(c.foreign_key->table + "(" + c.foreign_key->column + ")") : Value::null();
        r.rows.push_back({Value::text(c.name), Value::text(c.type_sql()), Value::text(c.not_null ? "NO" : "YES"),
                          Value::text(key), def, ref});
    }
    for (const auto& chk : t.schema().checks)
        r.rows.push_back({Value::text("CHECK"), Value::text(chk), Value::null(), Value::null(), Value::null(), Value::null()});
    r.tag = "DESCRIBE";
    return r;
}

}  // namespace atlas
