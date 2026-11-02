#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "common/error.h"
#include "sql/lexer.h"
#include "sql/parser.h"

using namespace atlas;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("lexer produces tokens with positions", "[lexer]") {
    auto toks = tokenize("SELECT a, 'it''s' FROM t -- comment\nWHERE x >= 1.5e2 /* block */ AND y <> 3;");
    REQUIRE(toks.front().kind == TokenKind::Identifier);
    REQUIRE(toks.front().text == "SELECT");
    bool saw_string = false, saw_float = false, saw_ne = false;
    for (const auto& t : toks) {
        if (t.kind == TokenKind::String) {
            saw_string = true;
            CHECK(t.text == "it's");
        }
        if (t.kind == TokenKind::Float) {
            saw_float = true;
            CHECK(t.text == "1.5e2");
            CHECK(t.line == 2);
        }
        if (t.kind == TokenKind::Symbol && t.text == "!=") saw_ne = true;
    }
    CHECK(saw_string);
    CHECK(saw_float);
    CHECK(saw_ne);
    CHECK(toks.back().kind == TokenKind::End);
}

TEST_CASE("lexer reports malformed input", "[lexer]") {
    CHECK_THROWS_AS(tokenize("SELECT 'unterminated"), DbError);
    CHECK_THROWS_AS(tokenize("SELECT /* never closed"), DbError);
    CHECK_THROWS_WITH(tokenize("SELECT @"), ContainsSubstring("unexpected character"));
    CHECK_THROWS_AS(tokenize("SELECT 12abc"), DbError);
}

TEST_CASE("parser handles a full SELECT", "[parser]") {
    auto st = parse_statement(
        "SELECT DISTINCT u.name AS n, COUNT(*) FROM users u LEFT JOIN orders o ON o.user_id = u.id "
        "WHERE u.age BETWEEN 18 AND 65 AND u.name LIKE 'A%' GROUP BY u.name HAVING COUNT(*) > 1 "
        "ORDER BY 2 DESC, n LIMIT 10 OFFSET 5");
    auto& s = std::get<SelectStmt>(st.node);
    CHECK(s.distinct);
    REQUIRE(s.items.size() == 2);
    CHECK(s.items[0].alias == "n");
    CHECK(s.items[1].expr->kind == ExprKind::Function);
    CHECK(s.items[1].expr->star);
    REQUIRE(s.from);
    CHECK(s.from->name == "users");
    CHECK(s.from->alias == "u");
    REQUIRE(s.joins.size() == 1);
    CHECK(s.joins[0].type == JoinType::Left);
    CHECK(s.joins[0].table.alias == "o");
    CHECK(s.where);
    CHECK(s.group_by.size() == 1);
    CHECK(s.having);
    REQUIRE(s.order_by.size() == 2);
    CHECK(s.order_by[0].desc);
    CHECK(s.limit);
    CHECK(s.offset);
}

TEST_CASE("parser handles CREATE TABLE constraints", "[parser]") {
    auto st = parse_statement(
        "CREATE TABLE IF NOT EXISTS emp ("
        " id INTEGER PRIMARY KEY,"
        " email VARCHAR(100) NOT NULL UNIQUE,"
        " salary REAL DEFAULT 1000.0 CHECK (salary > 0),"
        " dept INT REFERENCES dept(id),"
        " boss INT,"
        " FOREIGN KEY (boss) REFERENCES emp(id),"
        " CHECK (boss != id))");
    auto& c = std::get<CreateTableStmt>(st.node);
    CHECK(c.if_not_exists);
    REQUIRE(c.columns.size() == 5);
    CHECK(c.columns[0].primary_key);
    CHECK(c.columns[1].max_length == 100);
    CHECK(c.columns[1].not_null);
    CHECK(c.columns[1].unique);
    CHECK(c.columns[2].type == DataType::Real);
    CHECK(c.columns[2].default_value);
    CHECK(c.columns[2].check);
    REQUIRE(c.columns[3].references);
    CHECK(c.columns[3].references->table == "dept");
    REQUIRE(c.foreign_keys.size() == 1);
    CHECK(c.foreign_keys[0].first == "boss");
    CHECK(c.checks.size() == 1);
}

TEST_CASE("parser handles DML and transaction statements", "[parser]") {
    auto stmts = parse_sql(
        "BEGIN; INSERT INTO t (a, b) VALUES (1, 'x'), (2, NULL); UPDATE t SET a = a + 1 WHERE b IS NOT NULL;"
        "DELETE FROM t WHERE a IN (1, 2, 3); COMMIT; ROLLBACK; EXPLAIN SELECT * FROM t; SHOW TABLES; DESCRIBE t;");
    REQUIRE(stmts.size() == 9);
    CHECK(std::holds_alternative<BeginStmt>(stmts[0].node));
    auto& ins = std::get<InsertStmt>(stmts[1].node);
    CHECK(ins.columns.size() == 2);
    CHECK(ins.rows.size() == 2);
    CHECK(std::holds_alternative<UpdateStmt>(stmts[2].node));
    CHECK(std::holds_alternative<DeleteStmt>(stmts[3].node));
    CHECK(std::holds_alternative<CommitStmt>(stmts[4].node));
    CHECK(std::holds_alternative<RollbackStmt>(stmts[5].node));
    CHECK(std::holds_alternative<ExplainStmt>(stmts[6].node));
    CHECK(std::holds_alternative<ShowTablesStmt>(stmts[7].node));
    CHECK(std::holds_alternative<DescribeStmt>(stmts[8].node));
}

TEST_CASE("identifiers are case-insensitive unless quoted", "[parser]") {
    auto st = parse_statement("SELECT Name, \"MixedCase\" FROM Users");
    auto& s = std::get<SelectStmt>(st.node);
    CHECK(s.items[0].expr->name == "name");
    CHECK(s.items[1].expr->name == "MixedCase");
    CHECK(s.from->name == "users");
}

TEST_CASE("operator precedence", "[parser]") {
    CHECK(parse_expression("1 + 2 * 3")->to_string() == "(1 + (2 * 3))");
    CHECK(parse_expression("a OR b AND NOT c")->to_string() == "(a OR (b AND NOT c))");
    CHECK(parse_expression("-x * 2 = 4 OR y")->to_string() == "(((-x * 2) = 4) OR y)");
}

TEST_CASE("expressions round-trip through to_string", "[parser]") {
    const char* exprs[] = {
        "a + 1 > 2 AND b LIKE 'x%'",
        "x NOT IN (1, 2, NULL)",
        "price BETWEEN 1.5 AND 10",
        "name IS NOT NULL OR UPPER(name) = 'BOB'",
        "COALESCE(a, b, 'it''s')",
        "-5 + -2.5",
    };
    for (const char* src : exprs) {
        auto e = parse_expression(src);
        auto again = parse_expression(e->to_string());
        CHECK(again->to_string() == e->to_string());
    }
}

TEST_CASE("syntax errors report location", "[parser]") {
    CHECK_THROWS_WITH(parse_sql("SELECT FROM t"), ContainsSubstring("line 1"));
    CHECK_THROWS_WITH(parse_sql("SELECT a\nFROM t WHERE"), ContainsSubstring("line 2"));
    CHECK_THROWS_AS(parse_sql("CREATE TABLE t (a BLOB)"), DbError);
    CHECK_THROWS_AS(parse_sql("INSERT INTO t VALUES (1"), DbError);
    CHECK_THROWS_AS(parse_sql("SELECT 1 SELECT 2"), DbError);
    try {
        parse_sql("DELETE t");
        FAIL("expected an error");
    } catch (const DbError& e) {
        CHECK(e.code() == ErrorCode::Syntax);
    }
}

TEST_CASE("parameter placeholders", "[parser][params]") {
    auto st = parse_statement("SELECT * FROM t WHERE a = $2 AND b > $1");
    CHECK(count_parameters(st) == 2);
    auto q = parse_statement("INSERT INTO t VALUES (?, ?, ?)");
    CHECK(count_parameters(q) == 3);
    CHECK(count_parameters(parse_statement("SELECT 1")) == 0);
    CHECK_THROWS_AS(parse_statement("SELECT ? + $1"), DbError);
    CHECK_THROWS_AS(parse_statement("SELECT $0"), DbError);

    // Binding replaces placeholders with literals; the original stays intact in a clone.
    Statement copy = clone_statement(st);
    bind_parameters(copy, {Value::integer(1), Value::text("x")});
    CHECK(count_parameters(copy) == 0);
    CHECK(count_parameters(st) == 2);
    auto& where = *std::get<SelectStmt>(copy.node).where;
    CHECK(where.to_string() == "((a = 'x') AND (b > 1))");
    CHECK_THROWS_AS(bind_parameters(st, {Value::integer(1)}), DbError);
}
