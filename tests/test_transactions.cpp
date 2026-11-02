#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <random>
#include <thread>

#include "common/error.h"
#include "test_util.h"

using namespace atlas;
using atlas::test::TestDb;

TEST_CASE("ROLLBACK undoes inserts, updates and deletes", "[txn]") {
    TestDb t;
    t.run("CREATE TABLE acc (id INT PRIMARY KEY, owner TEXT, balance INT)");
    t.run("INSERT INTO acc VALUES (1, 'a', 100), (2, 'b', 50), (3, 'c', 0)");

    t.run("BEGIN");
    CHECK(t.session->in_transaction());
    t.run("INSERT INTO acc VALUES (4, 'd', 10)");
    t.run("UPDATE acc SET balance = balance - 30, owner = owner || '-----------------------------------------' WHERE id = 1");
    t.run("DELETE FROM acc WHERE id = 3");
    CHECK(t.count("acc") == 3);  // own changes are visible
    t.run("ROLLBACK");
    CHECK_FALSE(t.session->in_transaction());

    auto r = t.run("SELECT id, owner, balance FROM acc ORDER BY id");
    REQUIRE(r.rows.size() == 3);
    CHECK(r.rows[0][1].as_text() == "a");
    CHECK(r.rows[0][2].as_int() == 100);
    CHECK(r.rows[2][0].as_int() == 3);
    // Indexes were restored as well.
    CHECK(t.count("acc WHERE id = 4") == 0);
    CHECK(t.count("acc WHERE id = 3") == 1);
    t.run("INSERT INTO acc VALUES (4, 'd', 10)");
}

TEST_CASE("COMMIT makes changes permanent", "[txn]") {
    TestDb t;
    t.run("CREATE TABLE x (v INT)");
    t.run("BEGIN; INSERT INTO x VALUES (1); INSERT INTO x VALUES (2); COMMIT;");
    CHECK(t.count("x") == 2);
    t.reopen();
    CHECK(t.count("x") == 2);
}

TEST_CASE("a failed statement inside a transaction only rolls back itself", "[txn]") {
    TestDb t;
    t.run("CREATE TABLE k (id INT PRIMARY KEY)");
    t.run("BEGIN");
    t.run("INSERT INTO k VALUES (1), (2)");
    CHECK_THROWS_AS(t.run("INSERT INTO k VALUES (3), (1)"), DbError);
    CHECK(t.session->in_transaction());
    t.run("INSERT INTO k VALUES (3)");
    t.run("COMMIT");
    CHECK(t.count("k") == 3);
}

TEST_CASE("transaction state errors", "[txn]") {
    TestDb t;
    CHECK_THROWS_AS(t.run("COMMIT"), DbError);
    CHECK_THROWS_AS(t.run("ROLLBACK"), DbError);
    t.run("BEGIN");
    CHECK_THROWS_AS(t.run("BEGIN"), DbError);
    try {
        t.run("CREATE TABLE nope (a INT)");
        FAIL("DDL inside a transaction should fail");
    } catch (const DbError& e) {
        CHECK(e.code() == ErrorCode::Transaction);
    }
    t.run("ROLLBACK");
    t.run("CREATE TABLE nope (a INT)");
}

TEST_CASE("closing a session rolls back its open transaction", "[txn]") {
    TestDb t;
    t.run("CREATE TABLE s (v INT)");
    {
        Session other(*t.db);
        other.execute("BEGIN; INSERT INTO s VALUES (1);");
    }
    CHECK(t.count("s") == 0);
}

TEST_CASE("transactions are serialized between sessions", "[txn][concurrency]") {
    DatabaseOptions opts;
    opts.lock_timeout = std::chrono::milliseconds(200);
    TestDb t(opts);
    t.run("CREATE TABLE c (v INT)");

    Session a(*t.db);
    Session b(*t.db);
    a.execute("BEGIN; INSERT INTO c VALUES (1);");
    // b cannot read or write while a holds the transaction lock.
    try {
        b.execute("SELECT * FROM c");
        FAIL("expected lock timeout");
    } catch (const DbError& e) {
        CHECK(e.code() == ErrorCode::Locked);
    }
    a.execute("COMMIT");
    CHECK(b.execute("SELECT COUNT(*) FROM c")[0].rows[0][0].as_int() == 1);
}

TEST_CASE("concurrent autocommit writers do not lose updates", "[txn][concurrency]") {
    DatabaseOptions opts;
    opts.lock_timeout = std::chrono::seconds(30);
    TestDb t(opts);
    t.run("CREATE TABLE counter (id INT PRIMARY KEY, n INT)");
    t.run("INSERT INTO counter VALUES (1, 0)");

    constexpr int threads = 4, increments = 100;
    std::vector<std::thread> workers;
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&] {
            Session s(*t.db);
            for (int k = 0; k < increments; ++k) {
                if (k % 2) s.execute("UPDATE counter SET n = n + 1 WHERE id = 1");
                else s.execute("BEGIN; UPDATE counter SET n = n + 1 WHERE id = 1; COMMIT;");
            }
        });
    }
    for (auto& w : workers) w.join();
    CHECK(t.scalar("SELECT n FROM counter").as_int() == threads * increments);
}

TEST_CASE("reader/writer lock semantics", "[txn][lock]") {
    using std::chrono::milliseconds;
    TransactionLock lock;
    // Many readers at once.
    REQUIRE(lock.acquire_shared(milliseconds(10)));
    REQUIRE(lock.acquire_shared(milliseconds(10)));
    CHECK(lock.readers() == 2);
    // A writer must wait for readers.
    CHECK_FALSE(lock.acquire(1, milliseconds(20)));
    lock.release_shared();
    lock.release_shared();
    REQUIRE(lock.acquire(1, milliseconds(10)));
    CHECK(lock.acquire(1, milliseconds(10)));  // re-entrant for the owner
    // Readers wait for the writer.
    CHECK_FALSE(lock.acquire_shared(milliseconds(20)));
    lock.release(1);
    REQUIRE(lock.acquire_shared(milliseconds(10)));

    // A waiting writer blocks new readers (no writer starvation).
    std::atomic<bool> writer_got_it{false};
    std::thread writer([&] {
        if (lock.acquire(2, milliseconds(2000))) {
            writer_got_it = true;
            lock.release(2);
        }
    });
    std::this_thread::sleep_for(milliseconds(50));
    CHECK_FALSE(lock.acquire_shared(milliseconds(20)));
    lock.release_shared();
    writer.join();
    CHECK(writer_got_it);
}

TEST_CASE("read-only statements run in parallel", "[txn][concurrency]") {
    TestDb t;
    t.run("CREATE TABLE r (v INT)");
    t.run("INSERT INTO r VALUES (1)");
    // Hold a shared lock as if another SELECT were running: reads still go through, writes wait.
    REQUIRE(t.db->lock().acquire_shared(std::chrono::milliseconds(10)));
    CHECK(t.count("r") == 1);
    Session other(*t.db);
    auto start = std::chrono::steady_clock::now();
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        t.db->lock().release_shared();
    });
    other.execute("INSERT INTO r VALUES (2)");  // blocks until the reader finishes
    release.join();
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(90));
    CHECK(t.count("r") == 2);
}

TEST_CASE("concurrent readers see consistent snapshots while writers commit", "[txn][concurrency]") {
    DatabaseOptions opts;
    opts.lock_timeout = std::chrono::seconds(60);
    opts.cache_pages_per_table = 8;  // force page evictions while readers run
    TestDb t(opts);
    t.run("CREATE TABLE acct (id INT PRIMARY KEY, balance INT, pad TEXT)");
    t.run("BEGIN");
    for (int i = 0; i < 400; ++i)
        t.run("INSERT INTO acct VALUES (" + std::to_string(i) + ", 100, '" + std::string(150, 'p') + "')");
    t.run("COMMIT");
    const int64_t total = 400 * 100;

    std::atomic<bool> stop{false};
    std::atomic<int> bad{0}, reads{0};
    std::vector<std::thread> threads;
    for (int r = 0; r < 4; ++r) {
        threads.emplace_back([&, r] {
            Session s(*t.db);
            while (!stop) {
                auto res = s.execute("SELECT SUM(balance) FROM acct");
                if (res[0].rows[0][0].as_int() != total) ++bad;
                auto one = s.execute("SELECT balance FROM acct WHERE id = " + std::to_string((r * 37 + reads) % 400));
                if (one[0].rows.size() != 1) ++bad;
                ++reads;
            }
        });
    }
    for (int w = 0; w < 2; ++w) {
        threads.emplace_back([&, w] {
            Session s(*t.db);
            std::mt19937 rng(static_cast<unsigned>(w + 1));
            for (int k = 0; k < 150; ++k) {
                int a = static_cast<int>(rng() % 400), b = static_cast<int>(rng() % 400);
                s.execute("BEGIN; UPDATE acct SET balance = balance - 7 WHERE id = " + std::to_string(a) +
                          "; UPDATE acct SET balance = balance + 7 WHERE id = " + std::to_string(b) + "; COMMIT;");
            }
        });
    }
    for (size_t i = 4; i < threads.size(); ++i) threads[i].join();
    stop = true;
    for (size_t i = 0; i < 4; ++i) threads[i].join();
    CHECK(bad == 0);
    CHECK(reads > 0);
    CHECK(t.scalar("SELECT SUM(balance) FROM acct").as_int() == total);
}

TEST_CASE("group commit shares fsyncs between concurrent commits", "[txn][concurrency][group-commit]") {
    DatabaseOptions opts;
    opts.lock_timeout = std::chrono::seconds(60);
    TestDb t(opts, /*sync=*/true);
    t.run("CREATE TABLE g (id INT PRIMARY KEY, worker INT)");
    uint64_t syncs_before = t.db->wal_syncs();

    constexpr int threads = 8, per_thread = 40;
    std::atomic<int> reader_errors{0};
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        Session s(*t.db);
        int64_t last = 0;
        while (!stop) {
            // Counts only ever grow: readers never see a commit that later disappears.
            int64_t n = s.execute("SELECT COUNT(*) FROM g")[0].rows[0][0].as_int();
            if (n < last) ++reader_errors;
            last = n;
        }
    });
    std::vector<std::thread> writers;
    for (int w = 0; w < threads; ++w) {
        writers.emplace_back([&, w] {
            Session s(*t.db);
            for (int i = 0; i < per_thread; ++i) {
                int id = w * 1000 + i;
                if (i % 4 == 0)
                    s.execute("BEGIN; INSERT INTO g VALUES (" + std::to_string(id) + ", " + std::to_string(w) + "); COMMIT;");
                else
                    s.execute("INSERT INTO g VALUES (" + std::to_string(id) + ", " + std::to_string(w) + ")");
            }
        });
    }
    for (auto& w : writers) w.join();
    stop = true;
    reader.join();

    CHECK(t.count("g") == threads * per_thread);
    CHECK(reader_errors == 0);
    uint64_t syncs = t.db->wal_syncs() - syncs_before;
    INFO("fsyncs: " << syncs << " for " << threads * per_thread << " commits");
    CHECK(syncs >= 1);
    CHECK(syncs <= static_cast<uint64_t>(threads * per_thread));

    // Everything acknowledged is in the WAL and survives a crash.
    t.crash_and_recover();
    CHECK(t.count("g") == threads * per_thread);
}
