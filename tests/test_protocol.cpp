#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <thread>

#include "common/error.h"
#include "net/client.h"
#include "net/protocol.h"
#include "net/server.h"
#include "test_util.h"

using namespace atlas;
using atlas::test::TempDir;
using atlas::params;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("protocol messages round-trip", "[protocol]") {
    QueryResult r;
    r.has_rows = true;
    r.columns = {"id", "name", "score", "ok"};
    r.types = {DataType::Integer, DataType::Text, DataType::Real, DataType::Boolean};
    r.rows = {{Value::integer(1), Value::text("ana"), Value::real(9.5), Value::boolean(true)},
              {Value::integer(2), Value::null(), Value::real(-1), Value::boolean(false)}};
    r.tag = "SELECT 2";
    auto [type, body] = proto::encode_result(r);
    CHECK(type == proto::MsgType::RowSet);
    QueryResult back = proto::decode_rowset(body);
    CHECK(back.columns == r.columns);
    CHECK(back.types == r.types);
    REQUIRE(back.rows.size() == 2);
    CHECK(back.rows[0][1].as_text() == "ana");
    CHECK(back.rows[1][1].is_null());
    CHECK(back.tag == "SELECT 2");

    QueryResult cmd;
    cmd.tag = "INSERT 3";
    cmd.affected = 3;
    auto [ctype, cbody] = proto::encode_result(cmd);
    CHECK(ctype == proto::MsgType::Complete);
    CHECK(proto::decode_complete(cbody).affected == 3);

    DbError err = proto::decode_error(proto::encode_error(ErrorCode::Constraint, "boom"));
    CHECK(err.code() == ErrorCode::Constraint);
    CHECK(std::string(err.what()) == "boom");

    CHECK(proto::decode_hello(proto::encode_hello("me")) == "me");
    CHECK_THROWS_AS(proto::decode_hello("XXXX\x00\x01"), DbError);
    CHECK_THROWS_AS(proto::decode_rowset(body.substr(0, body.size() / 2)), DbError);
}

TEST_CASE("client and server talk over TCP", "[protocol][server]") {
    TempDir dir;
    DatabaseOptions dopts;
    dopts.sync_commits = false;
    dopts.lock_timeout = std::chrono::milliseconds(300);
    auto db = Database::open(dir.path, dopts);
    ServerOptions sopts;
    sopts.port = 0;  // ephemeral
    Server server(*db, sopts);
    server.start();
    REQUIRE(server.port() != 0);

    {
        auto c = Client::connect("127.0.0.1", server.port(), "test");
        CHECK_THAT(c.server_name(), ContainsSubstring("AtlasDB"));
        CHECK_FALSE(c.in_transaction());

        auto results = c.query("CREATE TABLE t (id INT PRIMARY KEY, s TEXT); INSERT INTO t VALUES (1, 'x'), (2, 'y');");
        REQUIRE(results.size() == 2);
        CHECK(results[1].affected == 2);

        auto r = c.query_one("SELECT id, s FROM t ORDER BY id DESC");
        REQUIRE(r.rows.size() == 2);
        CHECK(r.rows[0][0].as_int() == 2);
        CHECK(r.types[0] == DataType::Integer);

        // Errors carry the server-side error code; earlier statements still ran.
        try {
            c.query("INSERT INTO t VALUES (3, 'z'); INSERT INTO t VALUES (1, 'dup'); INSERT INTO t VALUES (4, 'w');");
            FAIL("expected constraint violation");
        } catch (const DbError& e) {
            CHECK(e.code() == ErrorCode::Constraint);
        }
        CHECK(c.query_one("SELECT COUNT(*) FROM t").rows[0][0].as_int() == 3);

        try {
            c.query("SELEC 1");
            FAIL("expected syntax error");
        } catch (const DbError& e) {
            CHECK(e.code() == ErrorCode::Syntax);
        }

        // Transaction status is reported back; a second client is blocked meanwhile.
        c.query("BEGIN; DELETE FROM t;");
        CHECK(c.in_transaction());
        auto other = Client::connect("127.0.0.1", server.port());
        try {
            other.query("SELECT * FROM t");
            FAIL("expected lock timeout");
        } catch (const DbError& e) {
            CHECK(e.code() == ErrorCode::Locked);
        }
        c.query("ROLLBACK");
        CHECK_FALSE(c.in_transaction());
        CHECK(other.query_one("SELECT COUNT(*) FROM t").rows[0][0].as_int() == 3);
    }

    {
        // A client that disconnects mid-transaction has its work rolled back.
        auto c = Client::connect("127.0.0.1", server.port());
        c.query("BEGIN; INSERT INTO t VALUES (100, 'temp');");
    }
    {
        auto c = Client::connect("127.0.0.1", server.port());
        // Give the server a moment to notice the closed connection and release the lock.
        QueryResult r;
        for (int attempt = 0; attempt < 20; ++attempt) {
            try {
                r = c.query_one("SELECT COUNT(*) FROM t WHERE id = 100");
                break;
            } catch (const DbError& e) {
                if (e.code() != ErrorCode::Locked) throw;
            }
        }
        REQUIRE_FALSE(r.rows.empty());
        CHECK(r.rows[0][0].as_int() == 0);
    }

    // Several clients in parallel.
    std::vector<std::thread> clients;
    for (int i = 0; i < 4; ++i) {
        clients.emplace_back([&, i] {
            auto c = Client::connect("127.0.0.1", server.port());
            for (int k = 0; k < 25; ++k) {
                int id = 1000 + i * 100 + k;
                for (int attempt = 0;; ++attempt) {
                    try {
                        c.query("INSERT INTO t VALUES (" + std::to_string(id) + ", 'p')");
                        break;
                    } catch (const DbError& e) {
                        if (e.code() != ErrorCode::Locked || attempt > 50) throw;
                    }
                }
            }
        });
    }
    for (auto& th : clients) th.join();
    auto c = Client::connect("127.0.0.1", server.port());
    CHECK(c.query_one("SELECT COUNT(*) FROM t").rows[0][0].as_int() == 3 + 100);

    server.stop();
}

TEST_CASE("idle transactions are rolled back and their connection closed", "[protocol][server]") {
    TempDir dir;
    DatabaseOptions dopts;
    dopts.sync_commits = false;
    dopts.lock_timeout = std::chrono::milliseconds(200);
    auto db = Database::open(dir.path, dopts);
    ServerOptions sopts;
    sopts.port = 0;
    sopts.idle_transaction_timeout = std::chrono::milliseconds(300);
    Server server(*db, sopts);
    server.start();

    auto idle = Client::connect("127.0.0.1", server.port());
    idle.query("CREATE TABLE t (v INT); BEGIN; INSERT INTO t VALUES (1);");
    REQUIRE(idle.in_transaction());

    // Without the timeout this client would block everyone else forever.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    auto other = Client::connect("127.0.0.1", server.port());
    CHECK(other.query_one("SELECT COUNT(*) FROM t").rows[0][0].as_int() == 0);

    try {
        idle.query("COMMIT");
        FAIL("expected the idle connection to be terminated");
    } catch (const DbError& e) {
        CHECK(e.code() == ErrorCode::Transaction);
        CHECK_THAT(e.what(), ContainsSubstring("idle"));
    }
    CHECK_FALSE(idle.connected());

    // Autocommit clients that are merely idle are left alone.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(other.query_one("SELECT COUNT(*) FROM t").rows[0][0].as_int() == 0);
    server.stop();
}

TEST_CASE("parameters and prepared statements over the wire", "[protocol][server][params]") {
    TempDir dir;
    DatabaseOptions dopts;
    dopts.sync_commits = false;
    auto db = Database::open(dir.path, dopts);
    ServerOptions sopts;
    sopts.port = 0;
    Server server(*db, sopts);
    server.start();

    auto c = Client::connect("127.0.0.1", server.port());
    c.query("CREATE TABLE p (id INT PRIMARY KEY, name TEXT, score REAL, ok BOOLEAN)");
    CHECK(c.query("INSERT INTO p VALUES ($1, $2, $3, $4)", params(1, "o'neil", 2.5, true)).affected == 1);
    auto r = c.query("SELECT name, score, ok FROM p WHERE id = ?", params(1));
    REQUIRE(r.rows.size() == 1);
    CHECK(r.rows[0][0].as_text() == "o'neil");
    CHECK(r.rows[0][2].as_bool());

    auto ins = c.prepare("INSERT INTO p (id, name) VALUES (?, ?)");
    CHECK(ins.param_count() == 2);
    for (int i = 2; i <= 50; ++i) ins.execute(params(i, "user" + std::to_string(i)));
    CHECK(c.query_one("SELECT COUNT(*) FROM p").rows[0][0].as_int() == 50);
    try {
        ins.execute(params(2, "dup"));
        FAIL("expected duplicate key");
    } catch (const DbError& e) {
        CHECK(e.code() == ErrorCode::Constraint);
    }
    ins.close();
    CHECK_THROWS_AS(ins.execute(params(99, "gone")), DbError);

    // A failed prepare must not leave the previous unnamed statement runnable.
    c.query("SELECT * FROM p WHERE id = $1", params(1));
    try {
        c.query("DELETE FROM p WHERE oops = $1 AND", params(1));
        FAIL("expected syntax error");
    } catch (const DbError& e) {
        CHECK(e.code() == ErrorCode::Syntax);
    }
    CHECK(c.query_one("SELECT COUNT(*) FROM p").rows[0][0].as_int() == 50);
    CHECK_FALSE(c.in_transaction());
    server.stop();
}
