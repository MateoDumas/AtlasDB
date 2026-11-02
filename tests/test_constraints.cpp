#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "common/error.h"
#include "test_util.h"

using namespace atlas;
using atlas::test::TestDb;
using Catch::Matchers::ContainsSubstring;

namespace {
ErrorCode code_of(TestDb& t, const std::string& sql) {
    try {
        t.run(sql);
    } catch (const DbError& e) {
        return e.code();
    }
    FAIL("statement unexpectedly succeeded: " << sql);
    return ErrorCode::Internal;
}
}  // namespace

TEST_CASE("PRIMARY KEY and UNIQUE", "[constraints]") {
    TestDb t;
    t.run("CREATE TABLE u (id INTEGER PRIMARY KEY, email TEXT UNIQUE, nick TEXT)");
    t.run("INSERT INTO u VALUES (1, 'a@x', 'a')");
    CHECK(code_of(t, "INSERT INTO u VALUES (1, 'b@x', 'b')") == ErrorCode::Constraint);
    CHECK(code_of(t, "INSERT INTO u VALUES (2, 'a@x', 'b')") == ErrorCode::Constraint);
    CHECK(code_of(t, "INSERT INTO u VALUES (NULL, 'c@x', 'c')") == ErrorCode::Constraint);
    // UNIQUE allows several NULLs.
    t.run("INSERT INTO u VALUES (2, NULL, 'b'), (3, NULL, 'c')");
    CHECK(t.count("u") == 3);
    // Updating a key onto an existing one fails; onto itself is fine.
    CHECK(code_of(t, "UPDATE u SET id = 1 WHERE id = 2") == ErrorCode::Constraint);
    t.run("UPDATE u SET id = 2 WHERE id = 2");
    t.run("UPDATE u SET id = 20 WHERE id = 2");
    CHECK(t.count("u WHERE id = 20") == 1);
    // A freed key can be reused.
    t.run("DELETE FROM u WHERE id = 1");
    t.run("INSERT INTO u VALUES (1, 'a@x', 'again')");
}

TEST_CASE("a failing multi-row INSERT is atomic", "[constraints]") {
    TestDb t;
    t.run("CREATE TABLE k (id INTEGER PRIMARY KEY)");
    t.run("INSERT INTO k VALUES (1)");
    CHECK_THROWS_AS(t.run("INSERT INTO k VALUES (2), (3), (1), (4)"), DbError);
    CHECK(t.count("k") == 1);
    CHECK(t.count("k WHERE id = 2") == 0);  // index was rolled back too
    t.run("INSERT INTO k VALUES (2)");
}

TEST_CASE("NOT NULL, CHECK, DEFAULT and VARCHAR length", "[constraints]") {
    TestDb t;
    t.run(
        "CREATE TABLE p (id INT PRIMARY KEY, name VARCHAR(5) NOT NULL, price REAL DEFAULT 9.5 CHECK (price >= 0),"
        " qty INT DEFAULT 1, CHECK (qty > 0 OR price = 0))");
    t.run("INSERT INTO p (id, name) VALUES (1, 'apple')");
    auto r = t.run("SELECT price, qty FROM p");
    CHECK(r.rows[0][0].as_real() == 9.5);
    CHECK(r.rows[0][1].as_int() == 1);

    CHECK(code_of(t, "INSERT INTO p (id) VALUES (2)") == ErrorCode::Constraint);
    CHECK(code_of(t, "INSERT INTO p (id, name) VALUES (2, 'banana')") == ErrorCode::Constraint);
    CHECK(code_of(t, "INSERT INTO p (id, name, price) VALUES (2, 'kiwi', -1)") == ErrorCode::Constraint);
    CHECK(code_of(t, "INSERT INTO p (id, name, qty) VALUES (2, 'kiwi', 0)") == ErrorCode::Constraint);
    t.run("INSERT INTO p (id, name, price, qty) VALUES (2, 'kiwi', 0, 0)");  // table CHECK satisfied by OR
    t.run("INSERT INTO p (id, name, price) VALUES (3, 'ñandú', NULL)");    // NULL passes CHECK; 5 characters
    CHECK(code_of(t, "UPDATE p SET price = -5 WHERE id = 1") == ErrorCode::Constraint);
    CHECK(t.scalar("SELECT price FROM p WHERE id = 1").as_real() == 9.5);
}

TEST_CASE("type checking on write", "[constraints]") {
    TestDb t;
    t.run("CREATE TABLE ty (i INTEGER, r REAL, s TEXT, b BOOLEAN)");
    t.run("INSERT INTO ty VALUES (1, 2, 'x', TRUE)");
    t.run("INSERT INTO ty VALUES (4.0, 2.5, 'y', FALSE)");  // 4.0 is an exact integer
    CHECK(code_of(t, "INSERT INTO ty VALUES (1.5, 1, 'z', TRUE)") == ErrorCode::Type);
    CHECK(code_of(t, "INSERT INTO ty VALUES ('1', 1, 'z', TRUE)") == ErrorCode::Type);
    CHECK(code_of(t, "INSERT INTO ty VALUES (1, 1, 2, TRUE)") == ErrorCode::Type);
    CHECK(code_of(t, "INSERT INTO ty VALUES (1, 1, 'z', 1)") == ErrorCode::Type);
    CHECK(code_of(t, "INSERT INTO ty VALUES (1, 2, 3)") == ErrorCode::Semantic);
    CHECK(t.scalar("SELECT TYPEOF(i) FROM ty WHERE s = 'y'").as_text() == "INTEGER");
}

TEST_CASE("FOREIGN KEY restricts inserts, updates and deletes", "[constraints][fk]") {
    TestDb t;
    t.run("CREATE TABLE parent (id INTEGER PRIMARY KEY, code TEXT UNIQUE)");
    t.run("CREATE TABLE child (id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent, pcode TEXT REFERENCES parent(code))");
    t.run("INSERT INTO parent VALUES (1, 'A'), (2, 'B')");
    t.run("INSERT INTO child VALUES (10, 1, 'A'), (11, NULL, NULL)");

    CHECK(code_of(t, "INSERT INTO child VALUES (12, 99, NULL)") == ErrorCode::Constraint);
    CHECK(code_of(t, "INSERT INTO child VALUES (12, NULL, 'Z')") == ErrorCode::Constraint);
    CHECK(code_of(t, "UPDATE child SET parent_id = 3 WHERE id = 10") == ErrorCode::Constraint);
    t.run("UPDATE child SET parent_id = 2 WHERE id = 10");

    CHECK_THROWS_WITH(t.run("DELETE FROM parent WHERE id = 2"), ContainsSubstring("still referenced"));
    CHECK(code_of(t, "UPDATE parent SET code = 'AA' WHERE id = 1") == ErrorCode::Constraint);
    t.run("UPDATE parent SET code = 'BB' WHERE id = 2");  // 'B' is not referenced
    t.run("DELETE FROM child WHERE id = 10");
    t.run("UPDATE parent SET code = 'AA' WHERE id = 1");
    t.run("DELETE FROM child");
    t.run("DELETE FROM parent WHERE id = 2");

    CHECK(code_of(t, "DROP TABLE parent") == ErrorCode::Constraint);
    t.run("DROP TABLE child");
    t.run("DROP TABLE parent");
}

TEST_CASE("FOREIGN KEY validation at CREATE TABLE", "[constraints][fk]") {
    TestDb t;
    t.run("CREATE TABLE a (id INT PRIMARY KEY, x INT, s TEXT UNIQUE)");
    CHECK(code_of(t, "CREATE TABLE b (r INT REFERENCES nowhere(id))") == ErrorCode::Semantic);
    CHECK(code_of(t, "CREATE TABLE b (r INT REFERENCES a(x))") == ErrorCode::Semantic);
    CHECK(code_of(t, "CREATE TABLE b (r INT REFERENCES a(s))") == ErrorCode::Semantic);
    CHECK(code_of(t, "CREATE TABLE b (r INT, PRIMARY KEY (r), q INT PRIMARY KEY)") == ErrorCode::Semantic);
    // Self reference: a row may point at itself or at an existing row.
    t.run("CREATE TABLE node (id INT PRIMARY KEY, parent INT REFERENCES node(id))");
    t.run("INSERT INTO node VALUES (1, 1), (2, 1)");
    CHECK(code_of(t, "INSERT INTO node VALUES (3, 4)") == ErrorCode::Constraint);
    CHECK(code_of(t, "DELETE FROM node WHERE id = 1") == ErrorCode::Constraint);
    t.run("DELETE FROM node WHERE id = 2");
    t.run("DELETE FROM node WHERE id = 1");  // only references itself
}

TEST_CASE("unique index creation validates existing data", "[constraints]") {
    TestDb t;
    t.run("CREATE TABLE d (v INT)");
    t.run("INSERT INTO d VALUES (1), (2), (2)");
    CHECK(code_of(t, "CREATE UNIQUE INDEX d_v ON d (v)") == ErrorCode::Constraint);
    t.run("DELETE FROM d WHERE v = 2");
    t.run("CREATE UNIQUE INDEX d_v ON d (v)");
    CHECK(code_of(t, "INSERT INTO d VALUES (1)") == ErrorCode::Constraint);
}
