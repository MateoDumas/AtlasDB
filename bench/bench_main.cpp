#include <benchmark/benchmark.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

#include "engine/database.h"
#include "engine/session.h"
#include "index/btree.h"
#include "sql/parser.h"

using namespace atlas;

namespace {

struct BenchDb {
    std::filesystem::path dir;
    std::unique_ptr<Database> db;
    std::unique_ptr<Session> session;

    explicit BenchDb(bool sync = false) {
        static int counter = 0;
        dir = std::filesystem::temp_directory_path() /
              ("atlasdb_bench_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
               std::to_string(counter++));
        DatabaseOptions opts;
        opts.sync_commits = sync;
        db = Database::open(dir, opts);
        session = std::make_unique<Session>(*db);
    }
    ~BenchDb() {
        session.reset();
        db.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void run(const std::string& sql) { session->execute(sql); }

    // users(id PK, age indexed, name) with n rows inserted in one transaction.
    void load_users(int64_t n) {
        run("CREATE TABLE users (id INTEGER PRIMARY KEY, age INTEGER, name TEXT, city TEXT)");
        run("CREATE INDEX users_age ON users (age)");
        run("BEGIN");
        for (int64_t i = 0; i < n; ++i)
            run("INSERT INTO users VALUES (" + std::to_string(i) + ", " + std::to_string(i % 90) + ", 'user" +
                std::to_string(i) + "', 'city" + std::to_string(i % 50) + "')");
        run("COMMIT");
    }
};

// ---- B+tree ---------------------------------------------------------------

void BM_BTreeInsertRandom(benchmark::State& state) {
    std::vector<int64_t> keys(static_cast<size_t>(state.range(0)));
    std::mt19937_64 rng(42);
    for (auto& k : keys) k = static_cast<int64_t>(rng());
    for (auto _ : state) {
        BPlusTree<int64_t> t(64);
        for (auto k : keys) t.insert(k);
        benchmark::DoNotOptimize(t.size());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_BTreeInsertRandom)->Arg(10'000)->Arg(100'000);

void BM_BTreeLookup(benchmark::State& state) {
    BPlusTree<int64_t> t(64);
    for (int64_t i = 0; i < state.range(0); ++i) t.insert(i * 3);
    std::mt19937_64 rng(7);
    for (auto _ : state) {
        int64_t k = static_cast<int64_t>(rng() % static_cast<uint64_t>(state.range(0) * 3));
        benchmark::DoNotOptimize(t.contains(k));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BTreeLookup)->Arg(100'000)->Arg(1'000'000);

// ---- SQL front-end ----------------------------------------------------------

void BM_ParseSelect(benchmark::State& state) {
    const std::string sql =
        "SELECT u.name, COUNT(*) AS n FROM users u JOIN orders o ON o.user_id = u.id "
        "WHERE u.age BETWEEN 20 AND 40 AND u.city IN ('a', 'b') GROUP BY u.name ORDER BY n DESC LIMIT 10";
    for (auto _ : state) benchmark::DoNotOptimize(parse_sql(sql));
}
BENCHMARK(BM_ParseSelect);

// ---- writes -------------------------------------------------------------------

void BM_InsertAutocommit(benchmark::State& state) {
    BenchDb b(state.range(0) != 0);
    b.run("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
    int64_t id = 0;
    for (auto _ : state) b.run("INSERT INTO t VALUES (" + std::to_string(id++) + ", 'value')");
    state.SetItemsProcessed(state.iterations());
    state.SetLabel(state.range(0) ? "fsync per commit" : "no fsync");
}
BENCHMARK(BM_InsertAutocommit)->Arg(0)->Arg(1)->Iterations(2000);

void BM_InsertInTransaction(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        auto b = std::make_unique<BenchDb>();
        b->run("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
        state.ResumeTiming();
        b->run("BEGIN");
        for (int64_t i = 0; i < state.range(0); ++i)
            b->run("INSERT INTO t VALUES (" + std::to_string(i) + ", 'value')");
        b->run("COMMIT");
        state.PauseTiming();
        b.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_InsertInTransaction)->Arg(10'000)->Unit(benchmark::kMillisecond);

// ---- reads --------------------------------------------------------------------

void BM_PointLookupIndexed(benchmark::State& state) {
    BenchDb b;
    b.load_users(state.range(0));
    std::mt19937_64 rng(1);
    for (auto _ : state) {
        auto id = rng() % static_cast<uint64_t>(state.range(0));
        benchmark::DoNotOptimize(b.session->execute("SELECT name FROM users WHERE id = " + std::to_string(id)));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PointLookupIndexed)->Arg(10'000)->Arg(100'000);

void BM_PointLookupFullScan(benchmark::State& state) {
    BenchDb b;
    b.load_users(state.range(0));
    std::mt19937_64 rng(1);
    for (auto _ : state) {
        auto id = rng() % static_cast<uint64_t>(state.range(0));
        // name has no index, so this scans the whole table.
        benchmark::DoNotOptimize(b.session->execute("SELECT id FROM users WHERE name = 'user" + std::to_string(id) + "'"));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PointLookupFullScan)->Arg(10'000)->Unit(benchmark::kMicrosecond);

void BM_RangeScanIndexed(benchmark::State& state) {
    BenchDb b;
    b.load_users(100'000);
    for (auto _ : state)
        benchmark::DoNotOptimize(b.session->execute("SELECT COUNT(*) FROM users WHERE age BETWEEN 30 AND 31"));
}
BENCHMARK(BM_RangeScanIndexed)->Unit(benchmark::kMicrosecond);

void BM_GroupBy(benchmark::State& state) {
    BenchDb b;
    b.load_users(100'000);
    for (auto _ : state)
        benchmark::DoNotOptimize(b.session->execute("SELECT city, COUNT(*), AVG(age) FROM users GROUP BY city"));
}
BENCHMARK(BM_GroupBy)->Unit(benchmark::kMillisecond);

void BM_IndexJoin(benchmark::State& state) {
    BenchDb b;
    b.load_users(10'000);
    b.run("CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER REFERENCES users(id), total REAL)");
    b.run("BEGIN");
    for (int i = 0; i < 20'000; ++i)
        b.run("INSERT INTO orders VALUES (" + std::to_string(i) + ", " + std::to_string(i % 10'000) + ", " +
              std::to_string(i % 97) + ")");
    b.run("COMMIT");
    for (auto _ : state)
        benchmark::DoNotOptimize(b.session->execute(
            "SELECT u.city, SUM(o.total) FROM orders o JOIN users u ON o.user_id = u.id GROUP BY u.city"));
}
BENCHMARK(BM_IndexJoin)->Unit(benchmark::kMillisecond);

}  // namespace

namespace {

void BM_PointLookupPrepared(benchmark::State& state) {
    BenchDb b;
    b.load_users(100'000);
    b.session->prepare("by_id", "SELECT name FROM users WHERE id = $1");
    std::mt19937_64 rng(1);
    for (auto _ : state) {
        auto id = static_cast<int64_t>(rng() % 100'000);
        benchmark::DoNotOptimize(b.session->execute_prepared("by_id", params(id)));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PointLookupPrepared);

}  // namespace

namespace {

// Time to open a 100k-row database (two indexes) and answer one indexed query.
// Indexes are loaded lazily from disk, so this does not grow with the table size.
void BM_OpenAndFirstQuery(benchmark::State& state) {
    BenchDb b;
    b.load_users(100'000);
    auto dir = b.dir;
    b.session.reset();
    b.db.reset();  // clean shutdown (checkpoint)
    for (auto _ : state) {
        auto db = Database::open(dir);
        Session s(*db);
        benchmark::DoNotOptimize(s.execute("SELECT name FROM users WHERE id = 54321"));
    }
    b.db = Database::open(dir);
    b.session = std::make_unique<Session>(*b.db);
}
BENCHMARK(BM_OpenAndFirstQuery)->Unit(benchmark::kMillisecond);

// For comparison: what startup used to cost, i.e. building the indexes from the rows.
void BM_RebuildIndexFromTable(benchmark::State& state) {
    BenchDb b;
    b.load_users(100'000);
    int n = 0;
    for (auto _ : state) {
        b.run("CREATE INDEX tmp" + std::to_string(n) + " ON users (id)");
        state.PauseTiming();
        b.run("DROP INDEX tmp" + std::to_string(n++));
        state.ResumeTiming();
    }
}
BENCHMARK(BM_RebuildIndexFromTable)->Unit(benchmark::kMillisecond);

}  // namespace
