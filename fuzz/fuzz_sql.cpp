// libFuzzer target: arbitrary bytes as SQL, run against a small database.
// Every input executes inside BEGIN ... ROLLBACK, so the database stays the same.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include "fuzz_inputs.h"

namespace {

atlas::Session& session() {
    static std::unique_ptr<atlas::Database> db;
    static std::unique_ptr<atlas::Session> s;
    if (!s) {
        auto dir = std::filesystem::temp_directory_path() / "atlasdb_fuzz_sql";
        std::filesystem::remove_all(dir);
        atlas::DatabaseOptions opts;
        opts.sync_commits = false;
        opts.lock_timeout = std::chrono::milliseconds(10);
        db = atlas::Database::open(dir, opts);
        s = std::make_unique<atlas::Session>(*db);
        s->execute(atlas::fuzz::SEED_SCHEMA);
    }
    return *s;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    atlas::fuzz::run_sql(session(), std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
