// Deterministic mutation fuzzing that runs everywhere as part of the test suite.
// The coverage-guided libFuzzer targets in fuzz/ go much deeper in CI.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <random>

#include "fuzz_inputs.h"
#include "test_util.h"

using namespace atlas;
using atlas::test::TestDb;

namespace {

const char* const FRAGMENTS[] = {
    "SELECT", " FROM ", " WHERE ", " AND ", " OR ", " NOT ", " NULL", " IS ", " IN (", ")", "(", ",", "*",
    " ORDER BY ", " GROUP BY ", " HAVING ", " LIMIT ", " OFFSET ", " JOIN ", " LEFT JOIN ", " ON ", " = ",
    " < ", " >= ", " != ", " + ", " - ", " / ", " % ", " || ", "'", "''", "\"", "--", "/*", "*/", ";",
    "9223372036854775807", "-9223372036854775808", "0", "1e308", ".5", "$1", "$99999", "?", "COUNT(*)",
    "SUM(", "AVG(DISTINCT ", "MIN(", "LENGTH(", "SUBSTR(", "a.", "b.", "id", "name", "score", "note",
    " BETWEEN ", " LIKE '%_'", " DISTINCT ", " DESC", " CASE ", "\xC3\xA9", "\xFF", "\x00",
};

// ATLAS_FUZZ_SEED / ATLAS_FUZZ_SCALE allow long local runs with other inputs.
unsigned seed_or(unsigned fallback) {
    const char* v = std::getenv("ATLAS_FUZZ_SEED");
    return v ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : fallback;
}
int scaled(int n) {
    const char* v = std::getenv("ATLAS_FUZZ_SCALE");
    return v ? n * std::max(1, std::atoi(v)) : n;
}

std::string mutate(std::string s, std::mt19937& rng) {
    int edits = 1 + static_cast<int>(rng() % 4);
    for (int e = 0; e < edits; ++e) {
        size_t pos = s.empty() ? 0 : rng() % (s.size() + 1);
        switch (rng() % 5) {
            case 0:  // insert a SQL fragment
                s.insert(pos, FRAGMENTS[rng() % std::size(FRAGMENTS)]);
                break;
            case 1:  // delete a span
                if (!s.empty()) s.erase(std::min(pos, s.size() - 1), 1 + rng() % 8);
                break;
            case 2:  // replace a byte
                if (!s.empty()) s[std::min(pos, s.size() - 1)] = static_cast<char>(rng() % 256);
                break;
            case 3:  // truncate
                s.resize(pos);
                break;
            default:  // duplicate a span
                if (!s.empty()) {
                    size_t start = std::min(pos, s.size() - 1);
                    s.insert(start, s.substr(start, 1 + rng() % 16));
                }
        }
    }
    return s;
}

}  // namespace

TEST_CASE("fuzz: mutated SQL never crashes the parser or the engine", "[fuzz]") {
    TestDb t;
    t.run(fuzz::SEED_SCHEMA);
    int64_t rows_before = t.count("a") + t.count("b");
    std::mt19937 rng(seed_or(20261102));
    for (int i = 0; i < scaled(5000); ++i) {
        std::string sql = mutate(fuzz::SEED_SQL[rng() % std::size(fuzz::SEED_SQL)], rng);
        INFO("input: " << sql);
        fuzz::run_sql(*t.session, sql);
        REQUIRE_FALSE(t.session->in_transaction());
    }
    // Every input ran inside a rolled-back transaction.
    CHECK(t.count("a") + t.count("b") == rows_before);
}

TEST_CASE("fuzz: random and corrupted protocol messages are rejected cleanly", "[fuzz][protocol]") {
    std::mt19937 rng(seed_or(7));
    // Valid messages of every type, to corrupt.
    QueryResult rs;
    rs.has_rows = true;
    rs.columns = {"a", "b"};
    rs.types = {DataType::Integer, DataType::Text};
    rs.rows = {{Value::integer(1), Value::text("x")}, {Value::null(), Value::real(2.5)}};
    rs.tag = "SELECT 2";
    std::vector<std::string> seeds = {
        proto::encode_hello("client"),
        proto::encode_hello_ok("server"),
        proto::encode_query("SELECT 1"),
        proto::encode_prepare("s1", "SELECT $1"),
        proto::encode_prepared(3),
        proto::encode_execute("s1", params(1, "two", 3.0, true, nullptr)),
        proto::encode_result(rs).second,
        proto::encode_error(ErrorCode::Constraint, "boom"),
        proto::encode_ready(true),
    };
    for (int i = 0; i < scaled(50000); ++i) {
        std::string body;
        if (rng() % 3 == 0) {
            body.resize(rng() % 64);
            for (auto& c : body) c = static_cast<char>(rng() % 256);
        } else {
            body = seeds[rng() % seeds.size()];
            int edits = 1 + static_cast<int>(rng() % 3);
            for (int e = 0; e < edits && !body.empty(); ++e) {
                size_t pos = rng() % body.size();
                if (rng() % 2) body[pos] = static_cast<char>(rng() % 256);
                else body.resize(pos);
            }
        }
        fuzz::decode_protocol(body);
    }
    SUCCEED();
}
