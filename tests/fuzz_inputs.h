#pragma once

// Shared by the Catch2 fuzz test and the libFuzzer targets: a database with a
// little data, and functions that push arbitrary input through the engine and
// the wire-protocol decoders. Anything other than a DbError is a bug.

#include <memory>
#include <string>
#include <string_view>

#include "common/error.h"
#include "engine/database.h"
#include "engine/session.h"
#include "net/protocol.h"
#include "sql/parser.h"

namespace atlas::fuzz {

inline const char* const SEED_SCHEMA =
    "CREATE TABLE a (id INT PRIMARY KEY, name TEXT UNIQUE, score REAL CHECK (score >= 0), flag BOOLEAN DEFAULT TRUE);"
    "CREATE TABLE b (id INT PRIMARY KEY, a_id INT REFERENCES a(id), note VARCHAR(20));"
    "CREATE INDEX a_score ON a (score);"
    "INSERT INTO a VALUES (1, 'x', 1.5, TRUE), (2, 'y', 2.5, FALSE), (3, NULL, 0, NULL);"
    "INSERT INTO b VALUES (10, 1, 'n1'), (11, 2, NULL), (12, NULL, 'n3');";

// Statements used as seeds for mutation.
inline const char* const SEED_SQL[] = {
    "SELECT * FROM a WHERE id = 1",
    "SELECT name, COUNT(*) FROM a GROUP BY name HAVING COUNT(*) > 0 ORDER BY 2 DESC LIMIT 3",
    "SELECT a.name, b.note FROM a LEFT JOIN b ON b.a_id = a.id WHERE a.score BETWEEN 0 AND 3",
    "SELECT DISTINCT flag FROM a ORDER BY flag",
    "SELECT UPPER(name) || '-' || SUBSTR(note, 1, 2) FROM a, b WHERE a.id = b.a_id",
    "SELECT id FROM a WHERE name LIKE 'x%' OR score IN (1.5, 2.5) OR flag IS NULL",
    "INSERT INTO a (id, name, score) VALUES (4, 'z', 3.5)",
    "INSERT INTO b SELECT id + 100, id, name FROM a",
    "UPDATE a SET score = score * 2, name = COALESCE(name, 'n') WHERE id > 1",
    "DELETE FROM b WHERE a_id IS NOT NULL",
    "EXPLAIN SELECT * FROM a WHERE score > 1 ORDER BY score DESC LIMIT 1",
    "SELECT ROUND(AVG(score), 2), MIN(id), MAX(name), SUM(id) FROM a",
    "SELECT * FROM a WHERE id = $1",
    "CREATE TABLE c (x INT PRIMARY KEY)",
};

// Runs one SQL text inside BEGIN ... ROLLBACK so the database never changes.
inline void run_sql(Session& s, std::string_view sql) {
    try {
        auto stmts = parse_sql(sql);
        s.execute("BEGIN");
        try {
            for (auto& st : stmts) {
                // Keep each input cheap and the shared database stable.
                if (std::holds_alternative<CheckpointStmt>(st.node)) continue;
                try {
                    s.execute_statement(st);
                } catch (const DbError&) {
                }
                if (!s.in_transaction()) s.execute("BEGIN");  // input ran COMMIT/ROLLBACK itself
            }
        } catch (...) {
            if (s.in_transaction()) s.execute("ROLLBACK");
            throw;
        }
        if (s.in_transaction()) s.execute("ROLLBACK");
    } catch (const DbError&) {
    }
}

// Feeds arbitrary bytes to every protocol decoder.
inline void decode_protocol(std::string_view body) {
    auto attempt = [&](auto&& f) {
        try {
            f();
        } catch (const DbError&) {
        }
    };
    attempt([&] { proto::decode_hello(body); });
    attempt([&] { proto::decode_hello_ok(body); });
    attempt([&] { proto::decode_query(body); });
    attempt([&] { proto::decode_prepare(body); });
    attempt([&] { proto::decode_prepared(body); });
    attempt([&] { proto::decode_execute(body); });
    attempt([&] { proto::decode_rowset(body); });
    attempt([&] { proto::decode_complete(body); });
    attempt([&] { proto::decode_error(body); });
    attempt([&] { proto::decode_ready(body); });
}

}  // namespace atlas::fuzz
