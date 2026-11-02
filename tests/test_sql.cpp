#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "common/error.h"
#include "test_util.h"

using namespace atlas;
using atlas::test::TestDb;
using atlas::params;
using Catch::Matchers::ContainsSubstring;

namespace {

void seed(TestDb& t) {
    t.run(
        "CREATE TABLE dept (id INTEGER PRIMARY KEY, name TEXT NOT NULL);"
        "CREATE TABLE emp (id INTEGER PRIMARY KEY, name TEXT NOT NULL, dept_id INTEGER REFERENCES dept(id),"
        "                  salary REAL, active BOOLEAN DEFAULT TRUE);"
        "INSERT INTO dept VALUES (1, 'Engineering'), (2, 'Sales'), (3, 'Empty');"
        "INSERT INTO emp (id, name, dept_id, salary) VALUES"
        "  (1, 'Ana', 1, 5000), (2, 'Bruno', 1, 4200.5), (3, 'Carla', 2, 3900),"
        "  (4, 'Diego', 2, NULL), (5, 'Eva', NULL, 3100);");
}

std::vector<std::string> column(const QueryResult& r, size_t c = 0) {
    std::vector<std::string> out;
    for (const auto& row : r.rows) out.push_back(row[c].to_string());
    return out;
}

}  // namespace

TEST_CASE("basic insert and select", "[sql]") {
    TestDb t;
    seed(t);
    auto r = t.run("SELECT * FROM emp ORDER BY id");
    REQUIRE(r.has_rows);
    CHECK(r.columns == std::vector<std::string>{"id", "name", "dept_id", "salary", "active"});
    CHECK(r.rows.size() == 5);
    CHECK(r.rows[0][1].as_text() == "Ana");
    CHECK(r.rows[0][3].type() == DataType::Real);  // INTEGER literal widened to REAL column
    CHECK(r.rows[0][4].as_bool());                 // DEFAULT TRUE
    CHECK(r.rows[3][3].is_null());
    CHECK(r.tag == "SELECT 5");
}

TEST_CASE("WHERE with NULL semantics, LIKE, IN, BETWEEN", "[sql]") {
    TestDb t;
    seed(t);
    CHECK(column(t.run("SELECT name FROM emp WHERE salary > 4000 ORDER BY name")) ==
          std::vector<std::string>{"Ana", "Bruno"});
    CHECK(t.count("emp WHERE salary IS NULL") == 1);
    CHECK(t.scalar("SELECT COUNT(*) FROM emp WHERE salary != 3900").as_int() == 3);  // NULL excluded
    CHECK(column(t.run("SELECT name FROM emp WHERE name LIKE '_r%' ORDER BY 1")) ==
          std::vector<std::string>{"Bruno"});
    CHECK(t.scalar("SELECT COUNT(*) FROM emp WHERE id IN (1, 3, 99)").as_int() == 2);
    CHECK(t.scalar("SELECT COUNT(*) FROM emp WHERE id NOT IN (1, 3)").as_int() == 3);
    CHECK(t.scalar("SELECT COUNT(*) FROM emp WHERE id NOT IN (1, NULL)").as_int() == 0);
    CHECK(t.scalar("SELECT COUNT(*) FROM emp WHERE salary BETWEEN 3900 AND 5000").as_int() == 3);
    CHECK(t.scalar("SELECT COUNT(*) FROM emp WHERE NOT (salary > 4000) OR salary IS NULL").as_int() == 3);
}

TEST_CASE("expressions and scalar functions", "[sql]") {
    TestDb t;
    CHECK(t.scalar("SELECT 1 + 2 * 3").as_int() == 7);
    CHECK(t.scalar("SELECT 7 / 2").as_int() == 3);
    CHECK(t.scalar("SELECT 7 / 2.0").as_real() == 3.5);
    CHECK(t.scalar("SELECT 7 % 3").as_int() == 1);
    CHECK(t.scalar("SELECT 'a' || 'b' || 1").as_text() == "ab1");
    CHECK(t.scalar("SELECT UPPER('abc')").as_text() == "ABC");
    CHECK(t.scalar("SELECT LENGTH('héllo')").as_int() == 5);
    CHECK(t.scalar("SELECT ABS(-4)").as_int() == 4);
    CHECK(t.scalar("SELECT ROUND(2.567, 2)").as_real() == 2.57);
    CHECK(t.scalar("SELECT SUBSTR('database', 5, 4)").as_text() == "base");
    CHECK(t.scalar("SELECT COALESCE(NULL, NULL, 3)").as_int() == 3);
    CHECK(t.scalar("SELECT NULL + 1").is_null());
    CHECK(t.scalar("SELECT NULL = NULL").is_null());
    CHECK_THROWS_WITH(t.run("SELECT 1 / 0"), ContainsSubstring("division by zero"));
    CHECK_THROWS_WITH(t.run("SELECT 9223372036854775807 + 1"), ContainsSubstring("overflow"));
    CHECK_THROWS_WITH(t.run("SELECT 'a' < 1"), ContainsSubstring("cannot compare"));
}

TEST_CASE("ORDER BY, LIMIT, OFFSET, DISTINCT", "[sql]") {
    TestDb t;
    seed(t);
    CHECK(column(t.run("SELECT name FROM emp ORDER BY salary DESC, name LIMIT 2")) ==
          std::vector<std::string>{"Ana", "Bruno"});
    CHECK(column(t.run("SELECT name FROM emp ORDER BY id LIMIT 2 OFFSET 3")) ==
          std::vector<std::string>{"Diego", "Eva"});
    // ORDER BY a column that is not selected.
    CHECK(column(t.run("SELECT name FROM emp ORDER BY dept_id DESC, id")) ==
          std::vector<std::string>{"Carla", "Diego", "Ana", "Bruno", "Eva"});
    // ORDER BY an alias.
    CHECK(column(t.run("SELECT name, salary * 2 AS double_pay FROM emp WHERE salary IS NOT NULL ORDER BY double_pay")) ==
          std::vector<std::string>{"Eva", "Carla", "Bruno", "Ana"});
    CHECK(t.run("SELECT DISTINCT dept_id FROM emp").rows.size() == 3);
    CHECK_THROWS_AS(t.run("SELECT DISTINCT dept_id FROM emp ORDER BY salary"), DbError);
}

TEST_CASE("aggregates and GROUP BY / HAVING", "[sql]") {
    TestDb t;
    seed(t);
    auto r = t.run("SELECT COUNT(*), COUNT(salary), SUM(salary), MIN(name), MAX(salary) FROM emp");
    REQUIRE(r.rows.size() == 1);
    CHECK(r.rows[0][0].as_int() == 5);
    CHECK(r.rows[0][1].as_int() == 4);
    CHECK(r.rows[0][2].as_real() == 16200.5);
    CHECK(r.rows[0][3].as_text() == "Ana");
    CHECK(r.rows[0][4].as_real() == 5000);

    r = t.run(
        "SELECT dept_id, COUNT(*) AS n, AVG(salary) FROM emp WHERE dept_id IS NOT NULL "
        "GROUP BY dept_id HAVING COUNT(*) >= 2 ORDER BY n DESC, dept_id");
    REQUIRE(r.rows.size() == 2);
    CHECK(r.rows[0][0].as_int() == 1);
    CHECK(r.rows[0][2].as_real() == (5000 + 4200.5) / 2);
    CHECK(r.rows[1][2].as_real() == 3900);  // AVG ignores NULL

    CHECK(t.scalar("SELECT COUNT(DISTINCT dept_id) FROM emp").as_int() == 2);
    CHECK(t.scalar("SELECT SUM(salary) FROM emp WHERE id > 100").is_null());
    CHECK(t.scalar("SELECT COUNT(*) FROM emp WHERE id > 100").as_int() == 0);
    CHECK_THROWS_WITH(t.run("SELECT name, COUNT(*) FROM emp"), ContainsSubstring("GROUP BY"));
    CHECK_THROWS_AS(t.run("SELECT * FROM emp WHERE COUNT(*) > 1"), DbError);
}

TEST_CASE("inner, left and cross joins", "[sql][join]") {
    TestDb t;
    seed(t);
    auto r = t.run("SELECT e.name, d.name FROM emp e JOIN dept d ON e.dept_id = d.id ORDER BY e.id");
    REQUIRE(r.rows.size() == 4);
    CHECK(r.rows[2][1].as_text() == "Sales");

    r = t.run("SELECT d.name, e.name FROM dept d LEFT JOIN emp e ON e.dept_id = d.id ORDER BY d.id, e.id");
    REQUIRE(r.rows.size() == 5);
    CHECK(r.rows[4][0].as_text() == "Empty");
    CHECK(r.rows[4][1].is_null());

    // WHERE on the right side of a LEFT JOIN filters after the join.
    r = t.run("SELECT d.name FROM dept d LEFT JOIN emp e ON e.dept_id = d.id WHERE e.id IS NULL");
    REQUIRE(r.rows.size() == 1);
    CHECK(r.rows[0][0].as_text() == "Empty");

    CHECK(t.run("SELECT * FROM dept, emp").rows.size() == 15);
    CHECK(t.run("SELECT * FROM dept d, emp e WHERE d.id = e.dept_id AND d.name = 'Sales'").rows.size() == 2);

    r = t.run(
        "SELECT d.name, COUNT(e.id) AS staff FROM dept d LEFT JOIN emp e ON e.dept_id = d.id "
        "GROUP BY d.name ORDER BY staff DESC, d.name");
    CHECK(column(r) == std::vector<std::string>{"Engineering", "Sales", "Empty"});
    CHECK(column(r, 1) == std::vector<std::string>{"2", "2", "0"});

    CHECK_THROWS_WITH(t.run("SELECT id FROM emp e JOIN dept d ON e.dept_id = d.id"), ContainsSubstring("ambiguous"));
    CHECK_THROWS_AS(t.run("SELECT * FROM emp e JOIN dept e ON 1 = 1"), DbError);
}

TEST_CASE("planner picks indexes", "[sql][planner]") {
    TestDb t;
    seed(t);
    t.run("CREATE INDEX emp_salary ON emp (salary)");
    auto plan = [&](const std::string& q) {
        std::string s;
        for (const auto& row : t.run("EXPLAIN " + q).rows) s += row[0].as_text() + "\n";
        return s;
    };
    CHECK_THAT(plan("SELECT * FROM emp WHERE id = 3"), ContainsSubstring("IndexScan emp USING emp_pkey"));
    CHECK_THAT(plan("SELECT * FROM emp WHERE 3 = id"), ContainsSubstring("emp_pkey"));
    CHECK_THAT(plan("SELECT * FROM emp WHERE salary > 4000 AND salary <= 5000"), ContainsSubstring("emp_salary"));
    CHECK_THAT(plan("SELECT * FROM emp WHERE id IN (1, 2)"), ContainsSubstring("emp_pkey"));
    CHECK_THAT(plan("SELECT * FROM emp WHERE name = 'Ana'"), ContainsSubstring("SeqScan emp"));
    CHECK_THAT(plan("SELECT * FROM emp e JOIN dept d ON e.dept_id = d.id"), ContainsSubstring("IndexJoin dept"));
    CHECK_THAT(plan("UPDATE emp SET salary = 1 WHERE id = 1"), ContainsSubstring("IndexScan"));
    CHECK_THAT(plan("DELETE FROM emp WHERE salary < 4000"), ContainsSubstring("emp_salary"));

    // Index scans return the same rows as the equivalent full scan.
    CHECK(t.count("emp WHERE salary > 4000 AND salary <= 5000") == 2);
    CHECK(t.count("emp WHERE salary BETWEEN 3100 AND 3900") == 2);
    CHECK(t.count("emp WHERE salary < 4000") == 2);
    CHECK(t.count("emp WHERE id IN (1, 2, 2, 42)") == 2);
    CHECK(t.count("emp WHERE id > 2 AND id >= 4") == 2);
}

TEST_CASE("UPDATE and DELETE", "[sql]") {
    TestDb t;
    seed(t);
    auto r = t.run("UPDATE emp SET salary = salary * 1.1, name = UPPER(name) WHERE dept_id = 1");
    CHECK(r.tag == "UPDATE 2");
    CHECK(r.affected == 2);
    CHECK(t.scalar("SELECT name FROM emp WHERE id = 1").as_text() == "ANA");
    CHECK(t.scalar("SELECT ROUND(salary, 2) FROM emp WHERE id = 1").as_real() == 5500.0);

    // Updates that grow rows so they no longer fit force row movement.
    t.run("UPDATE emp SET name = name || '-------------------------------------------------------------'");
    CHECK(t.count("emp WHERE name LIKE '%-----'") == 5);
    CHECK(t.scalar("SELECT name FROM emp WHERE id = 3").as_text().starts_with("Carla"));

    r = t.run("DELETE FROM emp WHERE salary IS NULL OR salary < 3500");
    CHECK(r.affected == 2);
    CHECK(t.count("emp") == 3);
    t.run("DELETE FROM emp");
    CHECK(t.count("emp") == 0);
}

TEST_CASE("INSERT ... SELECT copies rows", "[sql]") {
    TestDb t;
    seed(t);
    t.run("CREATE TABLE archive (id INTEGER PRIMARY KEY, name TEXT)");
    CHECK(t.run("INSERT INTO archive SELECT id, name FROM emp WHERE id <= 3").affected == 3);
    CHECK(t.count("archive") == 3);
    // Self-insert must not loop forever (rows are materialized first).
    t.run("CREATE TABLE nums (n INTEGER)");
    t.run("INSERT INTO nums VALUES (1), (2)");
    t.run("INSERT INTO nums SELECT n + 10 FROM nums");
    CHECK(t.count("nums") == 4);
}

TEST_CASE("catalog statements", "[sql]") {
    TestDb t;
    seed(t);
    auto r = t.run("SHOW TABLES");
    CHECK(column(r) == std::vector<std::string>{"dept", "emp"});
    r = t.run("DESCRIBE emp");
    CHECK(r.rows.size() == 5);
    CHECK(r.rows[0][3].as_text() == "PRIMARY");
    CHECK(r.rows[2][5].as_text() == "dept(id)");
    CHECK_THROWS_AS(t.run("SELECT * FROM missing"), DbError);
    CHECK_THROWS_AS(t.run("SELECT nope FROM emp"), DbError);
    CHECK_THROWS_AS(t.run("CREATE TABLE emp (x INT)"), DbError);
    t.run("CREATE TABLE IF NOT EXISTS emp (x INT)");
    t.run("CREATE INDEX emp_name ON emp (name)");
    CHECK_THROWS_AS(t.run("CREATE INDEX emp_name ON emp (name)"), DbError);
    t.run("DROP INDEX emp_name");
    CHECK_THROWS_AS(t.run("DROP INDEX emp_pkey"), DbError);
    t.run("DROP TABLE IF EXISTS nothing_here");
}

TEST_CASE("parameterized queries and prepared statements", "[sql][params]") {
    TestDb t;
    seed(t);
    auto& s = *t.session;
    auto r = s.execute("SELECT name FROM emp WHERE dept_id = $1 AND salary > $2 ORDER BY name",
                       params(1, 4300));
    CHECK(column(r) == std::vector<std::string>{"Ana"});

    // A hostile string is just a value.
    s.execute("INSERT INTO dept VALUES (?, ?)", params(9, "x'); DROP TABLE emp; --"));
    CHECK(t.scalar("SELECT name FROM dept WHERE id = 9").as_text() == "x'); DROP TABLE emp; --");
    CHECK(t.count("emp") == 5);

    CHECK(s.prepare("by_id", "SELECT name FROM emp WHERE id = $1") == 1);
    CHECK(s.execute_prepared("by_id", params(3)).rows[0][0].as_text() == "Carla");
    CHECK(s.execute_prepared("by_id", params(4)).rows[0][0].as_text() == "Diego");
    CHECK(s.execute_prepared("by_id", params(99)).rows.empty());
    CHECK_THROWS_AS(s.execute_prepared("by_id", {}), DbError);
    CHECK_THROWS_AS(s.execute_prepared("missing", params(1)), DbError);

    // Parameters reach the planner as constants, so indexes are still used.
    s.prepare("plan", "EXPLAIN SELECT * FROM emp WHERE id = $1");
    std::string plan;
    for (const auto& row : s.execute_prepared("plan", params(2)).rows) plan += row[0].as_text();
    CHECK_THAT(plan, ContainsSubstring("IndexScan emp USING emp_pkey"));

    // Prepared writes, NULL parameters and transactions.
    s.prepare("ins", "INSERT INTO emp (id, name, dept_id, salary) VALUES ($1, $2, $3, $4)");
    s.execute("BEGIN");
    s.execute_prepared("ins", params(10, "Fede", nullptr, 1000.5));
    s.execute_prepared("ins", params(11, "Gabi", 2, nullptr));
    CHECK_THROWS_AS(s.execute_prepared("ins", params(10, "dup", nullptr, nullptr)), DbError);
    s.execute("COMMIT");
    CHECK(t.count("emp") == 7);
    CHECK(t.scalar("SELECT salary FROM emp WHERE id = 10").as_real() == 1000.5);
    CHECK_THROWS_AS(s.execute_prepared("ins", params("not a number", "x", 1, 1)), DbError);

    s.deallocate("ins");
    CHECK_FALSE(s.has_prepared("ins"));
    CHECK_THROWS_WITH(t.run("SELECT * FROM emp WHERE id = $1"), ContainsSubstring("no values"));
}

TEST_CASE("output column names survive aggregation", "[sql]") {
    TestDb t;
    seed(t);
    auto r = t.run("SELECT d.name, COUNT(*) AS n, MAX(e.salary) FROM dept d JOIN emp e ON e.dept_id = d.id GROUP BY d.name");
    CHECK(r.columns == std::vector<std::string>{"name", "n", "MAX(e.salary)"});
}
