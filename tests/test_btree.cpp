#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <set>

#include "index/btree.h"
#include "index/index.h"
#include "test_util.h"

using namespace atlas;

TEST_CASE("B+tree basic insert, find, erase", "[btree]") {
    BPlusTree<int> t(4);
    for (int i = 0; i < 100; ++i) REQUIRE(t.insert(i * 2));
    CHECK(t.size() == 100);
    CHECK_FALSE(t.insert(10));
    CHECK(t.contains(42));
    CHECK_FALSE(t.contains(43));
    CHECK(t.height() > 2);
    CHECK(t.check_invariants());

    auto it = t.lower_bound(51);
    REQUIRE(it != t.end());
    CHECK(*it == 52);

    for (int i = 0; i < 100; i += 2) REQUIRE(t.erase(i * 2));
    CHECK_FALSE(t.erase(0));
    CHECK(t.size() == 50);
    CHECK(t.check_invariants());

    std::vector<int> all(t.begin(), t.end());
    CHECK(std::is_sorted(all.begin(), all.end()));
    CHECK(all.size() == 50);
}

TEST_CASE("B+tree matches std::set under random operations", "[btree]") {
    for (size_t order : {3u, 4u, 5u, 16u, 64u}) {
        BPlusTree<int> t(order);
        std::set<int> ref;
        std::mt19937 rng(static_cast<unsigned>(order) * 7919u);
        std::uniform_int_distribution<int> key(0, 2000);
        for (int step = 0; step < 20000; ++step) {
            int k = key(rng);
            if (rng() % 3 == 0) {
                REQUIRE(t.erase(k) == (ref.erase(k) == 1));
            } else {
                REQUIRE(t.insert(k) == ref.insert(k).second);
            }
            if (step % 1000 == 0) REQUIRE(t.check_invariants());
        }
        REQUIRE(t.check_invariants());
        REQUIRE(t.size() == ref.size());
        REQUIRE(std::equal(t.begin(), t.end(), ref.begin(), ref.end()));

        // Drain completely.
        std::vector<int> keys(ref.begin(), ref.end());
        std::shuffle(keys.begin(), keys.end(), rng);
        for (int k : keys) REQUIRE(t.erase(k));
        REQUIRE(t.empty());
        REQUIRE(t.check_invariants());
        REQUIRE(t.begin() == t.end());
    }
}

TEST_CASE("Index supports duplicates, lookups and ranges", "[btree][index]") {
    test::TempDir dir;
    Index idx(IndexDef{1, "i", "t", "c", false, false}, 0, dir.path / "i1.idx", 64);
    for (uint32_t i = 0; i < 1000; ++i) idx.insert(Value::integer(i % 100), RID{i, 0});
    idx.insert(Value::null(), RID{5000, 0});
    CHECK(idx.size() == 1001);
    CHECK(idx.lookup(Value::integer(42)).size() == 10);
    CHECK(idx.lookup(Value::integer(4200)).empty());
    CHECK(idx.lookup(Value::null()).empty());

    // 10 <= v < 20  -> 10 values * 10 copies
    auto r = idx.range(IndexBound{Value::integer(10), true}, IndexBound{Value::integer(20), false});
    CHECK(r.size() == 100);
    // v > 95 -> 96..99
    CHECK(idx.range(IndexBound{Value::integer(95), false}, std::nullopt).size() == 40);
    // v <= 2 never includes NULL
    CHECK(idx.range(std::nullopt, IndexBound{Value::integer(2), true}).size() == 30);
    // numeric comparison across INTEGER/REAL
    CHECK(idx.lookup(Value::real(7.0)).size() == 10);

    idx.erase(Value::integer(42), RID{42, 0});
    CHECK(idx.lookup(Value::integer(42)).size() == 9);
    CHECK(idx.find_other(Value::integer(42), RID{142, 0}).has_value());
}
