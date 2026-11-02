#include <catch2/catch_test_macros.hpp>

#include <array>
#include <map>
#include <random>

#include "storage/page.h"
#include "storage/row_codec.h"
#include "storage/table_heap.h"
#include "storage/wal.h"
#include "test_util.h"

using namespace atlas;
using atlas::test::TempDir;

TEST_CASE("slotted page insert/get/erase/update", "[storage][page]") {
    std::array<uint8_t, PAGE_SIZE> buf{};
    SlottedPage p(buf.data());
    p.init();
    CHECK(p.is_initialized());
    CHECK(p.slot_count() == 0);

    auto a = p.insert("hello");
    auto b = p.insert("world!");
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*p.get(*a) == "hello");
    CHECK(*p.get(*b) == "world!");

    p.erase(*a);
    CHECK_FALSE(p.get(*a));
    auto c = p.insert("again");  // reuses the empty slot
    REQUIRE(c);
    CHECK(*c == *a);

    CHECK(p.update(*b, "w"));
    CHECK(*p.get(*b) == "w");
    std::string big(1000, 'x');
    CHECK(p.update(*b, big));
    CHECK(*p.get(*b) == big);

    CHECK(p.put(10, "far"));
    CHECK(p.slot_count() == 11);
    CHECK(*p.get(10) == "far");
    CHECK_FALSE(p.get(7));

    p.set_lsn(1234);
    CHECK(p.lsn() == 1234);

    p.update_checksum();
    CHECK(p.verify_checksum());
    buf[PAGE_SIZE - 1] ^= 1;
    CHECK_FALSE(p.verify_checksum());
}

TEST_CASE("slotted page fills up and compacts", "[storage][page]") {
    std::array<uint8_t, PAGE_SIZE> buf{};
    SlottedPage p(buf.data());
    p.init();
    std::string rec(100, 'r');
    std::vector<uint16_t> slots;
    while (auto s = p.insert(rec)) slots.push_back(*s);
    CHECK(slots.size() > 30);
    CHECK(p.free_space() < 104);

    // Free every other record; a larger record must then fit only after compaction.
    for (size_t i = 0; i < slots.size(); i += 2) p.erase(slots[i]);
    std::string bigger(150, 'b');
    auto s = p.insert(bigger);
    REQUIRE(s);
    CHECK(*p.get(*s) == bigger);
    for (size_t i = 1; i < slots.size(); i += 2) CHECK(*p.get(slots[i]) == rec);

    CHECK_FALSE(p.insert(std::string(PAGE_SIZE, 'z')));
}

TEST_CASE("row codec round-trips all types", "[storage]") {
    Row row{Value::integer(-42), Value::real(3.25), Value::text("héllo"), Value::boolean(true), Value::null()};
    Row back = decode_row(encode_row(row));
    REQUIRE(back.size() == row.size());
    for (size_t i = 0; i < row.size(); ++i) {
        CHECK(back[i].type() == row[i].type());
        CHECK(back[i] == row[i]);
    }
}

TEST_CASE("table heap spans many pages and persists after flush", "[storage][heap]") {
    TempDir dir;
    auto path = dir.path / "heap.tbl";
    std::map<RID, std::string> expected;
    {
        TableHeap heap(path, 16);  // small cache to force eviction of clean pages
        for (int i = 0; i < 3000; ++i) {
            std::string rec = "record-" + std::to_string(i) + std::string(static_cast<size_t>(i % 50), '.');
            expected[heap.insert(rec)] = rec;
        }
        CHECK(heap.page_count() > 10);
        heap.flush();
        // Evictions only happen to clean pages; after the flush everything is clean.
        for (auto& [rid, rec] : expected) REQUIRE(*heap.get(rid) == rec);
    }
    {
        TableHeap heap(path, 16);
        size_t seen = 0;
        RID rid{0, 0};
        std::string rec;
        while (heap.next(rid, rid, rec)) {
            REQUIRE(expected.at(rid) == rec);
            ++seen;
            if (++rid.slot == 0) ++rid.page;
        }
        CHECK(seen == expected.size());
    }
}

TEST_CASE("WAL appends, reads back and truncates a torn tail", "[storage][wal]") {
    TempDir dir;
    auto path = dir.path / "wal.log";
    uint64_t good_size = 0;
    {
        Wal wal(path);
        std::vector<LogRecord> batch;
        for (uint64_t i = 1; i <= 5; ++i) {
            LogRecord r;
            r.lsn = i;
            r.type = i == 5 ? LogType::Commit : LogType::Put;
            r.table_id = 7;
            r.rid = RID{static_cast<uint32_t>(i), static_cast<uint16_t>(i)};
            r.payload = "payload" + std::to_string(i);
            batch.push_back(r);
        }
        wal.append(batch, true);
        good_size = wal.size();
    }
    {
        // Simulate a torn write: append half a frame of garbage.
        File f = File::open(path);
        std::string junk("\0\0\0\x40garbage", 11);
        f.write_at(f.size(), junk.data(), junk.size());
    }
    Wal wal(path);
    auto recs = wal.read_all();
    REQUIRE(recs.size() == 5);
    CHECK(recs[2].payload == "payload3");
    CHECK(recs[2].rid.page == 3);
    CHECK(recs[4].type == LogType::Commit);
    CHECK(wal.size() == good_size);
    Wal again(path);
    CHECK(again.read_all().size() == 5);
}
