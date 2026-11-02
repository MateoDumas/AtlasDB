#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <map>
#include <random>

#include "common/bytes.h"
#include "common/error.h"
#include "storage/file.h"
#include "storage/page.h"
#include "test_util.h"

using namespace atlas;
using atlas::test::TestDb;
using atlas::params;

TEST_CASE("data survives a clean restart", "[recovery]") {
    TestDb t;
    t.run("CREATE TABLE p (id INT PRIMARY KEY, name TEXT)");
    t.run("CREATE INDEX p_name ON p (name)");
    t.run("INSERT INTO p VALUES (1, 'one'), (2, 'two')");
    t.reopen();
    CHECK(t.count("p") == 2);
    CHECK(t.db->recovered_transactions() == 0);  // clean shutdown checkpointed everything
    // Explicit index definitions are persisted in the catalog.
    std::string plan;
    for (const auto& row : t.run("EXPLAIN SELECT * FROM p WHERE name = 'two'").rows) plan += row[0].as_text();
    CHECK(plan.find("p_name") != std::string::npos);
}

TEST_CASE("committed transactions are recovered from the WAL after a crash", "[recovery]") {
    TestDb t;
    t.run("CREATE TABLE r (id INT PRIMARY KEY, v TEXT)");
    t.run("INSERT INTO r VALUES (1, 'a'), (2, 'b')");
    t.run("BEGIN; UPDATE r SET v = 'B' WHERE id = 2; INSERT INTO r VALUES (3, 'c'); COMMIT;");
    t.run("DELETE FROM r WHERE id = 1");
    t.crash_and_recover();

    CHECK(t.db->recovered_transactions() >= 3);
    auto rows = t.run("SELECT id, v FROM r ORDER BY id").rows;
    REQUIRE(rows.size() == 2);
    CHECK(rows[0][1].as_text() == "B");
    CHECK(rows[1][0].as_int() == 3);
    // Indexes are rebuilt, so constraints still hold.
    CHECK_THROWS_AS(t.run("INSERT INTO r VALUES (3, 'dup')"), DbError);
}

TEST_CASE("uncommitted work is discarded by a crash", "[recovery]") {
    TestDb t;
    t.run("CREATE TABLE u (v INT)");
    t.run("INSERT INTO u VALUES (1)");
    t.run("BEGIN; INSERT INTO u VALUES (2); INSERT INTO u VALUES (3);");
    t.crash_and_recover();
    CHECK(t.count("u") == 1);
}

TEST_CASE("recovery after a checkpoint replays only newer changes", "[recovery]") {
    TestDb t;
    t.run("CREATE TABLE c (id INT PRIMARY KEY, n INT)");
    t.run("INSERT INTO c VALUES (1, 10), (2, 20)");
    t.run("CHECKPOINT");
    t.run("UPDATE c SET n = n + 1");
    t.run("INSERT INTO c VALUES (3, 30)");
    t.crash_and_recover();
    CHECK(t.scalar("SELECT SUM(n) FROM c").as_int() == 11 + 21 + 30);
    // Recovering twice (crash right after recovery) is idempotent.
    t.crash_and_recover();
    CHECK(t.scalar("SELECT SUM(n) FROM c").as_int() == 62);
}

TEST_CASE("schema changes are recovered", "[recovery]") {
    TestDb t;
    t.run("CREATE TABLE keep (id INT PRIMARY KEY)");
    t.run("CREATE TABLE gone (id INT)");
    t.run("INSERT INTO gone VALUES (1)");
    t.run("CHECKPOINT");
    t.run("DROP TABLE gone");
    t.run("CREATE TABLE fresh (id INT PRIMARY KEY, label TEXT)");
    t.run("CREATE INDEX fresh_label ON fresh (label)");
    t.run("INSERT INTO fresh VALUES (1, 'x')");
    t.run("INSERT INTO keep VALUES (7)");
    t.crash_and_recover();

    CHECK_THROWS_AS(t.run("SELECT * FROM gone"), DbError);
    CHECK(t.count("fresh WHERE label = 'x'") == 1);
    CHECK(t.count("keep") == 1);
    auto files = 0;
    for (const auto& e : std::filesystem::directory_iterator(t.dir.path))
        if (e.path().extension() == ".tbl") ++files;
    CHECK(files == 2);  // the dropped table's file was removed
}

TEST_CASE("a torn WAL tail is ignored", "[recovery]") {
    TestDb t;
    t.run("CREATE TABLE w (v INT)");
    t.run("INSERT INTO w VALUES (1)");
    t.run("INSERT INTO w VALUES (2)");
    t.db->simulate_crash();
    t.session.reset();
    t.db.reset();
    {
        // Chop the last few bytes off the log, as if power failed mid-write.
        auto path = t.dir.path / "wal.log";
        File f = File::open(path);
        uint64_t size = f.size();
        REQUIRE(size > 10);
        f.truncate(size - 7);
    }
    t.open();
    CHECK(t.count("w") == 1);
    t.run("INSERT INTO w VALUES (3)");
    t.crash_and_recover();
    CHECK(t.scalar("SELECT SUM(v) FROM w").as_int() == 4);
}

TEST_CASE("random workload survives repeated crashes", "[recovery]") {
    DatabaseOptions opts;
    opts.checkpoint_wal_bytes = 64 * 1024;  // checkpoint often to mix flushed and logged state
    opts.cache_pages_per_table = 8;
    TestDb t(opts);
    t.run("CREATE TABLE kv (k INT PRIMARY KEY, v TEXT)");
    std::map<int64_t, std::string> model;
    std::mt19937 rng(1234);

    for (int round = 0; round < 6; ++round) {
        for (int op = 0; op < 300; ++op) {
            int64_t k = rng() % 400;
            std::string v = std::string(rng() % 120, static_cast<char>('a' + rng() % 26));
            int choice = static_cast<int>(rng() % 3);
            bool in_txn = rng() % 5 == 0;
            bool commit = rng() % 2 == 0;
            if (in_txn) t.run("BEGIN");
            if (choice == 0 && !model.count(k)) {
                t.run("INSERT INTO kv VALUES (" + std::to_string(k) + ", '" + v + "')");
                if (!in_txn || commit) model[k] = v;
            } else if (choice == 1) {
                t.run("UPDATE kv SET v = '" + v + "' WHERE k = " + std::to_string(k));
                if ((!in_txn || commit) && model.count(k)) model[k] = v;
            } else {
                t.run("DELETE FROM kv WHERE k = " + std::to_string(k));
                if (!in_txn || commit) model.erase(k);
            }
            if (in_txn) t.run(commit ? "COMMIT" : "ROLLBACK");
        }
        t.crash_and_recover();
        auto rows = t.run("SELECT k, v FROM kv ORDER BY k").rows;
        REQUIRE(rows.size() == model.size());
        // The primary-key index (range scan) must agree with a full table scan.
        REQUIRE(t.count("kv WHERE k >= 0") == static_cast<int64_t>(model.size()));
        REQUIRE(t.count("kv WHERE k + 0 >= 0") == static_cast<int64_t>(model.size()));
        size_t i = 0;
        for (const auto& [k, v] : model) {
            REQUIRE(rows[i][0].as_int() == k);
            REQUIRE(rows[i][1].as_text() == v);
            ++i;
        }
    }
}

TEST_CASE("page checksums detect on-disk corruption", "[recovery][checksum]") {
    TestDb t;
    t.run("CREATE TABLE c (id INT PRIMARY KEY, body TEXT)");
    t.run("INSERT INTO c VALUES (1, 'precious data'), (2, 'more data')");
    t.close();  // clean shutdown writes the pages (with checksums)

    auto path = t.dir.path / "t1.tbl";
    REQUIRE(std::filesystem::file_size(path) == PAGE_SIZE);
    {
        File f = File::open(path);
        char byte;
        f.read_at(PAGE_SIZE - 5, &byte, 1);
        byte ^= 0x40;  // flip one bit inside a record
        f.write_at(PAGE_SIZE - 5, &byte, 1);
    }
    // Opening does not read table pages any more; the damage shows up on first access.
    t.open();
    try {
        t.run("SELECT body FROM c WHERE id = 1");
        FAIL("corruption went unnoticed");
    } catch (const DbError& e) {
        CHECK(e.code() == ErrorCode::Corruption);
        CHECK_THAT(e.what(), Catch::Matchers::ContainsSubstring("checksum"));
    }
}

namespace {
// Every index must contain exactly one entry per row (NULLs included).
void check_indexes_match_table(TestDb& t) {
    for (Table* table : t.db->tables()) {
        // Count by scanning the heap: row_count() itself is answered from an index.
        uint64_t rows = 0;
        RID rid{0, 0};
        Row row;
        while (table->next_row(rid, rid, row)) {
            ++rows;
            if (++rid.slot == 0) ++rid.page;
        }
        for (const auto& idx : table->indexes()) {
            INFO("index " << idx->def().name);
            CHECK(idx->size() == rows);
            CHECK(idx->check_invariants());
        }
    }
}
}  // namespace

TEST_CASE("indexes are persistent and not rebuilt at startup", "[recovery][index]") {
    TestDb t;
    t.run("CREATE TABLE big (id INT PRIMARY KEY, grp INT, name TEXT)");
    t.run("CREATE INDEX big_grp ON big (grp)");
    t.run("BEGIN");
    for (int i = 0; i < 3000; ++i)
        t.run("INSERT INTO big VALUES (" + std::to_string(i) + ", " + std::to_string(i % 7) + ", 'n" +
              std::to_string(i) + "')");
    t.run("COMMIT");
    t.run("DELETE FROM big WHERE id % 3 = 0");
    t.reopen();

    CHECK(std::filesystem::exists(t.dir.path / "i1.idx"));
    CHECK(std::filesystem::exists(t.dir.path / "i2.idx"));
    // Nothing is loaded until a query needs it.
    Index* pk = t.db->table("big").find_index("big_pkey");
    REQUIRE(pk);
    CHECK(pk->loaded_nodes() == 0);
    CHECK(t.scalar("SELECT name FROM big WHERE id = 2999").as_text() == "n2999");
    CHECK(pk->loaded_nodes() <= 4);  // only the root-to-leaf path
    CHECK(t.count("big WHERE grp = 3") == t.count("big WHERE grp + 0 = 3"));
    check_indexes_match_table(t);
}

TEST_CASE("index changes are recovered from the WAL", "[recovery][index]") {
    TestDb t;
    t.run("CREATE TABLE r (id INT PRIMARY KEY, tag TEXT)");
    t.run("CREATE INDEX r_tag ON r (tag)");
    t.run("INSERT INTO r VALUES (1, 'a'), (2, 'b'), (3, 'c')");
    t.run("CHECKPOINT");
    t.run("UPDATE r SET tag = 'z' WHERE id = 2");
    t.run("DELETE FROM r WHERE id = 3");
    t.run("INSERT INTO r VALUES (4, 'd')");
    t.run("CREATE INDEX r_tag2 ON r (tag)");  // index created after the checkpoint
    t.crash_and_recover();

    CHECK(t.count("r WHERE tag = 'z'") == 1);
    CHECK(t.count("r WHERE tag = 'b'") == 0);
    CHECK(t.count("r WHERE id = 3") == 0);
    CHECK(t.count("r WHERE id = 4") == 1);
    CHECK_THROWS_AS(t.run("INSERT INTO r VALUES (4, 'dup')"), DbError);
    check_indexes_match_table(t);
    t.run("DROP INDEX r_tag2");
    t.reopen();
    CHECK_FALSE(std::filesystem::exists(t.dir.path / "i3.idx"));
}

TEST_CASE("a checkpoint interrupted mid-write is repaired from the doublewrite buffer", "[recovery][doublewrite]") {
    TestDb t;
    t.run("CREATE TABLE d (id INT PRIMARY KEY, v TEXT)");
    for (int i = 0; i < 200; ++i)
        t.run("INSERT INTO d VALUES (" + std::to_string(i) + ", '" + std::string(50, 'x') + "')");
    t.close();

    // Fake a checkpoint that crashed after the doublewrite buffer was synced but
    // while pages were being written: keep a good copy of the buffer contents,
    // then tear a data page and an index page.
    auto dir = t.dir.path;
    std::string tbl, idx;
    {
        File f = File::open(dir / "t1.tbl");
        tbl.resize(f.size());
        f.read_at(0, tbl.data(), tbl.size());
        File g = File::open(dir / "i1.idx");
        idx.resize(g.size());
        g.read_at(0, idx.data(), idx.size());
    }
    std::string catalog;
    {
        File c = File::open(dir / "catalog.bin");
        catalog.resize(c.size());
        c.read_at(0, catalog.data(), catalog.size());
    }
    ByteWriter dw;
    dw.raw("ATDW", 4);
    dw.u64(0);
    uint32_t pages = static_cast<uint32_t>(tbl.size() / PAGE_SIZE + idx.size() / PAGE_SIZE);
    dw.u32(pages);
    for (size_t p = 0; p < tbl.size() / PAGE_SIZE; ++p) {
        dw.str("t1.tbl");
        dw.u32(static_cast<uint32_t>(p));
        dw.raw(tbl.data() + p * PAGE_SIZE, PAGE_SIZE);
    }
    for (size_t p = 0; p < idx.size() / PAGE_SIZE; ++p) {
        dw.str("i1.idx");
        dw.u32(static_cast<uint32_t>(p));
        dw.raw(idx.data() + p * PAGE_SIZE, PAGE_SIZE);
    }
    dw.str(catalog);
    dw.u32(crc32(dw.data().data(), dw.size()));
    {
        File f = File::open(dir / "doublewrite.buf");
        f.write_at(0, dw.data().data(), dw.size());
    }
    {
        // Torn writes: the second half of a page never made it.
        std::string zeros(PAGE_SIZE / 2, '\0');
        File f = File::open(dir / "t1.tbl");
        f.write_at(PAGE_SIZE + PAGE_SIZE / 2, zeros.data(), zeros.size());
        File g = File::open(dir / "i1.idx");
        g.write_at(PAGE_SIZE / 2, zeros.data(), zeros.size());
    }

    t.open();
    CHECK_FALSE(std::filesystem::exists(dir / "doublewrite.buf"));
    CHECK(t.count("d") == 200);
    CHECK(t.count("d WHERE id = 150") == 1);
    check_indexes_match_table(t);
}

TEST_CASE("an incomplete doublewrite buffer is ignored", "[recovery][doublewrite]") {
    TestDb t;
    t.run("CREATE TABLE e (id INT PRIMARY KEY)");
    t.run("INSERT INTO e VALUES (1), (2)");
    t.close();
    {
        File f = File::open(t.dir.path / "doublewrite.buf");
        std::string partial = "ATDW garbage that was cut off";
        f.write_at(0, partial.data(), partial.size());
    }
    t.open();
    CHECK(t.count("e") == 2);
    CHECK_FALSE(std::filesystem::exists(t.dir.path / "doublewrite.buf"));
}

namespace {
// Physical heap records (rows + overflow pointers + chunks), to detect leaked chunks.
size_t heap_records(Table& t) {
    size_t n = 0;
    RID rid{0, 0};
    std::string rec;
    while (t.heap().next(rid, rid, rec)) {
        ++n;
        if (++rid.slot == 0) ++rid.page;
    }
    return n;
}
std::string big(size_t n, char c) { return std::string(n, c); }
}  // namespace

TEST_CASE("rows larger than a page use overflow chunks", "[storage][overflow]") {
    TestDb t;
    auto& s = *t.session;
    t.run("CREATE TABLE doc (id INT PRIMARY KEY, body TEXT, tag TEXT)");
    s.execute("INSERT INTO doc VALUES ($1, $2, $3)", params(1, big(50'000, 'a'), "x"));
    s.execute("INSERT INTO doc VALUES ($1, $2, $3)", params(2, big(9'000, 'b'), "y"));
    s.execute("INSERT INTO doc VALUES ($1, $2, $3)", params(3, "small", "z"));
    Table& table = t.db->table("doc");
    CHECK(heap_records(table) > 3);  // chunks exist

    CHECK(t.scalar("SELECT LENGTH(body) FROM doc WHERE id = 1").as_int() == 50'000);
    CHECK(t.scalar("SELECT SUBSTR(body, 49999, 2) FROM doc WHERE id = 1").as_text() == "aa");
    CHECK(t.count("doc WHERE body LIKE 'bbb%'") == 1);  // full scan resolves chains too

    // Grow, shrink, and replace in both directions; no chunk may leak.
    s.execute("UPDATE doc SET body = $1 WHERE id = 3", params(big(30'000, 'c')));
    s.execute("UPDATE doc SET body = 'tiny' WHERE id = 1");
    s.execute("UPDATE doc SET tag = 'changed' WHERE id = 2");  // big row, small change
    CHECK(t.scalar("SELECT LENGTH(body) FROM doc WHERE id = 3").as_int() == 30'000);
    CHECK(t.scalar("SELECT body FROM doc WHERE id = 1").as_text() == "tiny");
    CHECK(t.scalar("SELECT LENGTH(body) || tag FROM doc WHERE id = 2").as_text() == "9000changed");
    s.execute("UPDATE doc SET body = 'x'");
    CHECK(heap_records(table) == 3);
    t.run("DELETE FROM doc");
    CHECK(heap_records(table) == 0);

    // Indexed large values work too (index nodes span several pages).
    t.run("CREATE INDEX doc_body ON doc (body)");
    s.execute("INSERT INTO doc VALUES ($1, $2, 't')", params(5, big(12'000, 'k')));
    CHECK(t.count("doc WHERE body = '" + big(12'000, 'k') + "'") == 1);

    try {
        s.execute("INSERT INTO doc VALUES ($1, $2, 't')", params(6, big(17u << 20, 'q')));
        FAIL("expected a row-too-large error");
    } catch (const DbError& e) {
        CHECK(e.code() == ErrorCode::Constraint);
    }
}

TEST_CASE("rollback restores overflow rows exactly", "[storage][overflow][txn]") {
    TestDb t;
    auto& s = *t.session;
    t.run("CREATE TABLE r (id INT PRIMARY KEY, body TEXT)");
    s.execute("INSERT INTO r VALUES (1, $1)", params(big(20'000, 'o')));
    s.execute("INSERT INTO r VALUES (2, 'short')");
    Table& table = t.db->table("r");
    size_t records = heap_records(table);

    t.run("BEGIN");
    s.execute("UPDATE r SET body = 'now small' WHERE id = 1");
    s.execute("UPDATE r SET body = $1 WHERE id = 2", params(big(15'000, 'n')));
    s.execute("INSERT INTO r VALUES (3, $1)", params(big(8'000, 'i')));
    t.run("DELETE FROM r WHERE id = 1");
    t.run("ROLLBACK");

    CHECK(heap_records(table) == records);
    CHECK(t.scalar("SELECT body FROM r WHERE id = 1").as_text() == big(20'000, 'o'));
    CHECK(t.scalar("SELECT body FROM r WHERE id = 2").as_text() == "short");
    CHECK(t.count("r") == 2);

    // Slots freed by the rolled-back work are reused by later inserts; recovery must
    // reproduce exactly the same layout (records were logged at their real slots).
    for (int i = 10; i < 20; ++i) s.execute("INSERT INTO r VALUES ($1, $2)", params(i, big(3'000 + i, 'p')));
    t.crash_and_recover();
    CHECK(t.scalar("SELECT body FROM r WHERE id = 1").as_text() == big(20'000, 'o'));
    CHECK(t.count("r") == 12);
    CHECK(t.scalar("SELECT LENGTH(body) FROM r WHERE id = 19").as_int() == 3'019);
    check_indexes_match_table(t);
}

TEST_CASE("random workload with large rows survives crashes", "[recovery][overflow]") {
    DatabaseOptions opts;
    opts.checkpoint_wal_bytes = 256 * 1024;
    opts.cache_pages_per_table = 16;
    TestDb t(opts);
    t.run("CREATE TABLE kv (k INT PRIMARY KEY, v TEXT)");
    std::map<int64_t, std::string> model;
    std::mt19937 rng(99);
    auto random_value = [&] {
        size_t len = rng() % 4 == 0 ? 2'000 + rng() % 25'000 : rng() % 300;
        return std::string(len, static_cast<char>('a' + rng() % 26));
    };
    for (int round = 0; round < 4; ++round) {
        for (int op = 0; op < 150; ++op) {
            int64_t k = rng() % 60;
            bool in_txn = rng() % 4 == 0;
            bool commit = rng() % 2 == 0;
            if (in_txn) t.run("BEGIN");
            std::string v = random_value();
            int choice = static_cast<int>(rng() % 3);
            if (choice == 0 && !model.count(k)) {
                t.session->execute("INSERT INTO kv VALUES ($1, $2)", params(k, v));
                if (!in_txn || commit) model[k] = v;
            } else if (choice == 1) {
                t.session->execute("UPDATE kv SET v = $1 WHERE k = $2", params(v, k));
                if ((!in_txn || commit) && model.count(k)) model[k] = v;
            } else {
                t.session->execute("DELETE FROM kv WHERE k = $1", params(k));
                if (!in_txn || commit) model.erase(k);
            }
            if (in_txn) t.run(commit ? "COMMIT" : "ROLLBACK");
        }
        t.crash_and_recover();
        auto rows = t.run("SELECT k, v FROM kv ORDER BY k").rows;
        REQUIRE(rows.size() == model.size());
        size_t i = 0;
        for (const auto& [k, v] : model) {
            REQUIRE(rows[i][0].as_int() == k);
            REQUIRE(rows[i][1].as_text() == v);
            ++i;
        }
        check_indexes_match_table(t);
    }
}
