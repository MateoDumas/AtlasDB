#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "common/result.h"
#include "engine/database.h"
#include "engine/session.h"

namespace atlas::test {

// A fresh directory under the system temp dir, removed on destruction.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        static std::atomic<int> counter{0};
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("atlasdb_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// Database + session opened on a temp directory.
struct TestDb {
    TempDir dir;
    DatabaseOptions options;
    std::unique_ptr<Database> db;
    std::unique_ptr<Session> session;

    explicit TestDb(DatabaseOptions opts = {}) : options(opts) {
        options.sync_commits = false;  // tests do not need real fsyncs
        open();
    }
    ~TestDb() { close(); }

    void open() {
        db = Database::open(dir.path, options);
        session = std::make_unique<Session>(*db);
    }
    void close() {
        session.reset();
        db.reset();
    }
    void reopen() {
        close();
        open();
    }
    // Drops everything in memory without checkpointing, then reopens (runs recovery).
    void crash_and_recover() {
        db->simulate_crash();
        session.reset();
        db.reset();
        open();
    }

    QueryResult run(const std::string& sql) {
        auto results = session->execute(sql);
        return results.empty() ? QueryResult{} : results.back();
    }
    Value scalar(const std::string& sql) {
        QueryResult r = run(sql);
        if (r.rows.empty() || r.rows[0].empty()) return Value::null();
        return r.rows[0][0];
    }
    int64_t count(const std::string& table) { return scalar("SELECT COUNT(*) FROM " + table).as_int(); }
};

}  // namespace atlas::test
