#include "sql/parser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <unordered_set>

#include "common/error.h"
#include "sql/lexer.h"

namespace atlas {

namespace {

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Words that may not be used as unquoted identifiers or implicit aliases.
const std::unordered_set<std::string>& reserved_words() {
    static const std::unordered_set<std::string> words = {
        "SELECT", "FROM",   "WHERE",  "GROUP",  "ORDER",   "BY",       "HAVING", "LIMIT",  "OFFSET",
        "INSERT", "INTO",   "VALUES", "UPDATE", "SET",     "DELETE",   "CREATE", "DROP",   "TABLE",
        "INDEX",  "ON",     "JOIN",   "INNER",  "LEFT",    "OUTER",    "CROSS",  "AS",     "AND",
        "OR",     "NOT",    "NULL",   "IS",     "IN",      "BETWEEN",  "LIKE",   "TRUE",   "FALSE",
        "ASC",    "DESC",   "DISTINCT", "PRIMARY", "UNIQUE", "REFERENCES", "FOREIGN", "CHECK",
        "DEFAULT", "UNION", "ALL", "CONSTRAINT",
    };
    return words;
}

class Parser {
public:
    explicit Parser(std::string_view sql) : toks_(tokenize(sql)) {}

    std::vector<Statement> parse_script() {
        std::vector<Statement> out;
        while (true) {
            while (accept_symbol(";")) {
            }
            if (at_end()) break;
            out.push_back(parse_stmt());
            if (!at_end() && !accept_symbol(";")) fail("expected ';' or end of input");
        }
        return out;
    }

    Statement parse_single() {
        while (accept_symbol(";")) {
        }
        if (at_end()) fail("empty statement");
        Statement s = parse_stmt();
        while (accept_symbol(";")) {
        }
        if (!at_end()) fail("unexpected input after statement");
        return s;
    }

    ExprPtr parse_standalone_expr() {
        ExprPtr e = parse_expr();
        if (!at_end()) fail("unexpected input after expression");
        return e;
    }

private:
    // ---- token helpers ---------------------------------------------------

    const Token& peek(size_t k = 0) const { return toks_[std::min(pos_ + k, toks_.size() - 1)]; }
    bool at_end() const { return peek().kind == TokenKind::End; }
    const Token& advance() { return toks_[pos_ < toks_.size() - 1 ? pos_++ : pos_]; }

    [[noreturn]] void fail(const std::string& msg) const {
        const Token& t = peek();
        std::string near = t.kind == TokenKind::End ? "end of input" : "'" + t.text + "'";
        throw DbError(ErrorCode::Syntax, msg + " near " + near + " (line " + std::to_string(t.line) +
                                             ", column " + std::to_string(t.column) + ")");
    }

    bool is_kw(const char* kw, size_t k = 0) const {
        const Token& t = peek(k);
        if (t.kind != TokenKind::Identifier || t.text.size() != std::char_traits<char>::length(kw)) return false;
        for (size_t i = 0; i < t.text.size(); ++i)
            if (std::toupper(static_cast<unsigned char>(t.text[i])) != kw[i]) return false;
        return true;
    }

    bool accept_kw(const char* kw) {
        if (!is_kw(kw)) return false;
        advance();
        return true;
    }

    void expect_kw(const char* kw) {
        if (!accept_kw(kw)) fail(std::string("expected ") + kw);
    }

    bool is_symbol(const char* s, size_t k = 0) const {
        return peek(k).kind == TokenKind::Symbol && peek(k).text == s;
    }

    bool accept_symbol(const char* s) {
        if (!is_symbol(s)) return false;
        advance();
        return true;
    }

    void expect_symbol(const char* s) {
        if (!accept_symbol(s)) fail(std::string("expected '") + s + "'");
    }

    bool is_identifier_token(size_t k = 0) const {
        const Token& t = peek(k);
        if (t.kind == TokenKind::QuotedIdentifier) return true;
        return t.kind == TokenKind::Identifier && !reserved_words().count(upper(t.text));
    }

    std::string identifier(const char* what = "identifier") {
        const Token& t = peek();
        if (t.kind == TokenKind::QuotedIdentifier) return advance().text;
        if (t.kind == TokenKind::Identifier && !reserved_words().count(upper(t.text))) return lower(advance().text);
        fail(std::string("expected ") + what);
    }

    int64_t integer_literal() {
        bool neg = accept_symbol("-");
        if (peek().kind != TokenKind::Integer) fail("expected integer");
        int64_t v = parse_int(advance().text);
        return neg ? -v : v;
    }

    int64_t parse_int(const std::string& s) {
        int64_t v = 0;
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc() || p != s.data() + s.size()) fail("integer literal out of range");
        return v;
    }

    // ---- statements ------------------------------------------------------

    Statement parse_stmt() {
        // '?' placeholders are numbered per statement.
        next_positional_ = 0;
        saw_positional_ = saw_numbered_ = false;
        if (is_kw("SELECT")) return {parse_select()};
        if (is_kw("INSERT")) return {parse_insert()};
        if (is_kw("UPDATE")) return {parse_update()};
        if (is_kw("DELETE")) return {parse_delete()};
        if (is_kw("CREATE")) return parse_create();
        if (is_kw("DROP")) return parse_drop();
        if (accept_kw("BEGIN") || accept_kw("START")) {
            if (!accept_kw("TRANSACTION")) accept_kw("WORK");
            return {BeginStmt{}};
        }
        if (accept_kw("COMMIT") || accept_kw("END")) {
            if (!accept_kw("TRANSACTION")) accept_kw("WORK");
            return {CommitStmt{}};
        }
        if (accept_kw("ROLLBACK") || accept_kw("ABORT")) {
            if (!accept_kw("TRANSACTION")) accept_kw("WORK");
            return {RollbackStmt{}};
        }
        if (accept_kw("CHECKPOINT")) return {CheckpointStmt{}};
        if (accept_kw("SHOW")) {
            expect_kw("TABLES");
            return {ShowTablesStmt{}};
        }
        if (accept_kw("DESCRIBE") || accept_kw("DESC")) {
            accept_kw("TABLE");
            return {DescribeStmt{identifier("table name")}};
        }
        if (accept_kw("EXPLAIN")) {
            if (!is_kw("SELECT") && !is_kw("UPDATE") && !is_kw("DELETE"))
                fail("EXPLAIN supports SELECT, UPDATE and DELETE");
            ExplainStmt e;
            e.inner = std::make_unique<Statement>(parse_stmt());
            return {std::move(e)};
        }
        fail("expected a statement");
    }

    Statement parse_create() {
        expect_kw("CREATE");
        bool unique = accept_kw("UNIQUE");
        if (accept_kw("INDEX")) return {parse_create_index(unique)};
        if (unique) fail("expected INDEX after CREATE UNIQUE");
        expect_kw("TABLE");
        return {parse_create_table()};
    }

    bool parse_if_not_exists() {
        if (!is_kw("IF")) return false;
        advance();
        expect_kw("NOT");
        expect_kw("EXISTS");
        return true;
    }

    bool parse_if_exists() {
        if (!is_kw("IF")) return false;
        advance();
        expect_kw("EXISTS");
        return true;
    }

    CreateIndexStmt parse_create_index(bool unique) {
        CreateIndexStmt s;
        s.unique = unique;
        s.if_not_exists = parse_if_not_exists();
        s.name = identifier("index name");
        expect_kw("ON");
        s.table = identifier("table name");
        expect_symbol("(");
        s.column = identifier("column name");
        if (is_symbol(",")) fail("multi-column indexes are not supported");
        expect_symbol(")");
        return s;
    }

    DataType parse_type(uint32_t& max_len) {
        if (peek().kind != TokenKind::Identifier) fail("expected column type");
        std::string t = upper(advance().text);
        max_len = 0;
        DataType dt;
        if (t == "INT" || t == "INTEGER" || t == "BIGINT" || t == "SMALLINT") {
            dt = DataType::Integer;
        } else if (t == "REAL" || t == "FLOAT" || t == "DOUBLE" || t == "NUMERIC" || t == "DECIMAL") {
            if (t == "DOUBLE") accept_kw("PRECISION");
            dt = DataType::Real;
        } else if (t == "TEXT" || t == "VARCHAR" || t == "CHAR" || t == "STRING") {
            dt = DataType::Text;
            if ((t == "VARCHAR" || t == "CHAR") && accept_symbol("(")) {
                int64_t n = integer_literal();
                if (n <= 0 || n > 4000) fail("invalid VARCHAR length");
                max_len = static_cast<uint32_t>(n);
                expect_symbol(")");
            }
        } else if (t == "BOOL" || t == "BOOLEAN") {
            dt = DataType::Boolean;
        } else {
            --pos_;
            fail("unknown column type");
        }
        // Ignore precision/scale such as NUMERIC(10, 2).
        if (dt == DataType::Real && accept_symbol("(")) {
            integer_literal();
            if (accept_symbol(",")) integer_literal();
            expect_symbol(")");
        }
        return dt;
    }

    ForeignKeyRef parse_references() {
        ForeignKeyRef fk;
        fk.table = identifier("referenced table");
        if (accept_symbol("(")) {
            fk.column = identifier("referenced column");
            expect_symbol(")");
        }
        return fk;
    }

    std::string parse_single_column_list(const char* what) {
        expect_symbol("(");
        std::string col = identifier("column name");
        if (is_symbol(",")) fail(std::string("composite ") + what + " is not supported");
        expect_symbol(")");
        return col;
    }

    CreateTableStmt parse_create_table() {
        CreateTableStmt s;
        s.if_not_exists = parse_if_not_exists();
        s.name = identifier("table name");
        expect_symbol("(");
        do {
            if (accept_kw("CONSTRAINT")) identifier("constraint name");
            if (accept_kw("PRIMARY")) {
                expect_kw("KEY");
                s.primary_key.push_back(parse_single_column_list("PRIMARY KEY"));
            } else if (accept_kw("UNIQUE")) {
                s.unique.push_back(parse_single_column_list("UNIQUE constraint"));
            } else if (accept_kw("FOREIGN")) {
                expect_kw("KEY");
                std::string col = parse_single_column_list("FOREIGN KEY");
                expect_kw("REFERENCES");
                s.foreign_keys.emplace_back(col, parse_references());
            } else if (accept_kw("CHECK")) {
                expect_symbol("(");
                s.checks.push_back(parse_expr());
                expect_symbol(")");
            } else {
                s.columns.push_back(parse_column_def());
            }
        } while (accept_symbol(","));
        expect_symbol(")");
        return s;
    }

    ColumnDefAst parse_column_def() {
        ColumnDefAst c;
        c.name = identifier("column name");
        c.type = parse_type(c.max_length);
        while (true) {
            if (accept_kw("CONSTRAINT")) identifier("constraint name");
            if (accept_kw("PRIMARY")) {
                expect_kw("KEY");
                c.primary_key = true;
            } else if (accept_kw("NOT")) {
                expect_kw("NULL");
                c.not_null = true;
            } else if (accept_kw("NULL")) {
                // explicit nullable, nothing to do
            } else if (accept_kw("UNIQUE")) {
                c.unique = true;
            } else if (accept_kw("DEFAULT")) {
                c.default_value = parse_unary();
            } else if (accept_kw("CHECK")) {
                expect_symbol("(");
                c.check = parse_expr();
                expect_symbol(")");
            } else if (accept_kw("REFERENCES")) {
                c.references = parse_references();
            } else {
                break;
            }
        }
        return c;
    }

    Statement parse_drop() {
        expect_kw("DROP");
        if (accept_kw("TABLE")) {
            DropTableStmt s;
            s.if_exists = parse_if_exists();
            s.name = identifier("table name");
            return {s};
        }
        if (accept_kw("INDEX")) {
            DropIndexStmt s;
            s.if_exists = parse_if_exists();
            s.name = identifier("index name");
            return {s};
        }
        fail("expected TABLE or INDEX after DROP");
    }

    InsertStmt parse_insert() {
        expect_kw("INSERT");
        expect_kw("INTO");
        InsertStmt s;
        s.table = identifier("table name");
        if (accept_symbol("(")) {
            do s.columns.push_back(identifier("column name"));
            while (accept_symbol(","));
            expect_symbol(")");
        }
        if (is_kw("SELECT")) {
            s.select = std::make_unique<SelectStmt>(parse_select());
            return s;
        }
        expect_kw("VALUES");
        do {
            expect_symbol("(");
            std::vector<ExprPtr> row;
            do row.push_back(parse_expr());
            while (accept_symbol(","));
            expect_symbol(")");
            s.rows.push_back(std::move(row));
        } while (accept_symbol(","));
        return s;
    }

    UpdateStmt parse_update() {
        expect_kw("UPDATE");
        UpdateStmt s;
        s.table = identifier("table name");
        expect_kw("SET");
        do {
            std::string col = identifier("column name");
            expect_symbol("=");
            s.assignments.emplace_back(col, parse_expr());
        } while (accept_symbol(","));
        if (accept_kw("WHERE")) s.where = parse_expr();
        return s;
    }

    DeleteStmt parse_delete() {
        expect_kw("DELETE");
        expect_kw("FROM");
        DeleteStmt s;
        s.table = identifier("table name");
        if (accept_kw("WHERE")) s.where = parse_expr();
        return s;
    }

    TableRef parse_table_ref() {
        TableRef t;
        t.name = identifier("table name");
        if (accept_kw("AS")) {
            t.alias = identifier("alias");
        } else if (is_identifier_token()) {
            t.alias = identifier();
        }
        return t;
    }

    SelectStmt parse_select() {
        expect_kw("SELECT");
        SelectStmt s;
        if (accept_kw("DISTINCT")) s.distinct = true;
        else accept_kw("ALL");

        do {
            SelectItem item;
            if (accept_symbol("*")) {
                item.star = true;
            } else if (is_identifier_token() && is_symbol(".", 1) && is_symbol("*", 2)) {
                item.star = true;
                item.star_table = identifier();
                advance();
                advance();
            } else {
                item.expr = parse_expr();
                if (accept_kw("AS")) item.alias = identifier("alias");
                else if (is_identifier_token()) item.alias = identifier();
            }
            s.items.push_back(std::move(item));
        } while (accept_symbol(","));

        if (accept_kw("FROM")) {
            s.from = parse_table_ref();
            while (true) {
                JoinClause j;
                if (accept_symbol(",")) {
                    j.type = JoinType::Cross;
                    j.table = parse_table_ref();
                } else if (accept_kw("CROSS")) {
                    expect_kw("JOIN");
                    j.type = JoinType::Cross;
                    j.table = parse_table_ref();
                } else if (is_kw("JOIN") || is_kw("INNER") || is_kw("LEFT")) {
                    if (accept_kw("LEFT")) {
                        accept_kw("OUTER");
                        j.type = JoinType::Left;
                    } else {
                        accept_kw("INNER");
                        j.type = JoinType::Inner;
                    }
                    expect_kw("JOIN");
                    j.table = parse_table_ref();
                    expect_kw("ON");
                    j.on = parse_expr();
                } else {
                    break;
                }
                s.joins.push_back(std::move(j));
            }
        }
        if (accept_kw("WHERE")) s.where = parse_expr();
        if (accept_kw("GROUP")) {
            expect_kw("BY");
            do s.group_by.push_back(parse_expr());
            while (accept_symbol(","));
        }
        if (accept_kw("HAVING")) s.having = parse_expr();
        if (accept_kw("ORDER")) {
            expect_kw("BY");
            do {
                OrderItem o;
                o.expr = parse_expr();
                if (accept_kw("DESC")) o.desc = true;
                else accept_kw("ASC");
                s.order_by.push_back(std::move(o));
            } while (accept_symbol(","));
        }
        if (accept_kw("LIMIT")) {
            s.limit = parse_expr();
            if (accept_kw("OFFSET")) s.offset = parse_expr();
            else if (accept_symbol(",")) {
                // LIMIT offset, count (MySQL style)
                s.offset = std::move(s.limit);
                s.limit = parse_expr();
            }
        } else if (accept_kw("OFFSET")) {
            s.offset = parse_expr();
        }
        return s;
    }

    // ---- expressions -----------------------------------------------------

    ExprPtr parse_expr() { return parse_or(); }

    ExprPtr parse_or() {
        ExprPtr e = parse_and();
        while (accept_kw("OR")) e = Expr::make_binary(BinaryOp::Or, std::move(e), parse_and());
        return e;
    }

    ExprPtr parse_and() {
        ExprPtr e = parse_not();
        while (accept_kw("AND")) e = Expr::make_binary(BinaryOp::And, std::move(e), parse_not());
        return e;
    }

    ExprPtr parse_not() {
        if (accept_kw("NOT")) return Expr::make_unary(UnaryOp::Not, parse_not());
        return parse_predicate();
    }

    ExprPtr parse_predicate() {
        ExprPtr left = parse_additive();
        while (true) {
            static const std::pair<const char*, BinaryOp> cmp[] = {
                {"=", BinaryOp::Eq}, {"!=", BinaryOp::Ne}, {"<", BinaryOp::Lt},
                {"<=", BinaryOp::Le}, {">", BinaryOp::Gt}, {">=", BinaryOp::Ge},
            };
            bool matched = false;
            for (auto& [sym, op] : cmp) {
                if (accept_symbol(sym)) {
                    left = Expr::make_binary(op, std::move(left), parse_additive());
                    matched = true;
                    break;
                }
            }
            if (matched) continue;

            if (accept_kw("IS")) {
                auto e = std::make_unique<Expr>();
                e->kind = ExprKind::IsNull;
                e->negated = accept_kw("NOT");
                expect_kw("NULL");
                e->args.push_back(std::move(left));
                left = std::move(e);
                continue;
            }

            bool negated = false;
            if (is_kw("NOT") && (is_kw("IN", 1) || is_kw("LIKE", 1) || is_kw("BETWEEN", 1))) {
                advance();
                negated = true;
            }
            if (accept_kw("IN")) {
                auto e = std::make_unique<Expr>();
                e->kind = ExprKind::In;
                e->negated = negated;
                e->args.push_back(std::move(left));
                expect_symbol("(");
                do e->args.push_back(parse_expr());
                while (accept_symbol(","));
                expect_symbol(")");
                left = std::move(e);
            } else if (accept_kw("LIKE")) {
                auto e = std::make_unique<Expr>();
                e->kind = ExprKind::Like;
                e->negated = negated;
                e->args.push_back(std::move(left));
                e->args.push_back(parse_additive());
                left = std::move(e);
            } else if (accept_kw("BETWEEN")) {
                auto e = std::make_unique<Expr>();
                e->kind = ExprKind::Between;
                e->negated = negated;
                e->args.push_back(std::move(left));
                e->args.push_back(parse_additive());
                expect_kw("AND");
                e->args.push_back(parse_additive());
                left = std::move(e);
            } else {
                if (negated) fail("expected IN, LIKE or BETWEEN after NOT");
                return left;
            }
        }
    }

    ExprPtr parse_additive() {
        ExprPtr e = parse_multiplicative();
        while (true) {
            if (accept_symbol("+")) e = Expr::make_binary(BinaryOp::Add, std::move(e), parse_multiplicative());
            else if (accept_symbol("-")) e = Expr::make_binary(BinaryOp::Sub, std::move(e), parse_multiplicative());
            else if (accept_symbol("||")) e = Expr::make_binary(BinaryOp::Concat, std::move(e), parse_multiplicative());
            else return e;
        }
    }

    ExprPtr parse_multiplicative() {
        ExprPtr e = parse_unary();
        while (true) {
            if (accept_symbol("*")) e = Expr::make_binary(BinaryOp::Mul, std::move(e), parse_unary());
            else if (accept_symbol("/")) e = Expr::make_binary(BinaryOp::Div, std::move(e), parse_unary());
            else if (accept_symbol("%")) e = Expr::make_binary(BinaryOp::Mod, std::move(e), parse_unary());
            else return e;
        }
    }

    ExprPtr parse_unary() {
        if (accept_symbol("-")) {
            // Fold negative numeric literals directly so that INT64_MIN parses.
            if (peek().kind == TokenKind::Integer) {
                std::string text = "-" + advance().text;
                return Expr::make_literal(Value::integer(parse_int(text)));
            }
            if (peek().kind == TokenKind::Float)
                return Expr::make_literal(Value::real(-std::strtod(advance().text.c_str(), nullptr)));
            return Expr::make_unary(UnaryOp::Neg, parse_unary());
        }
        if (accept_symbol("+")) return parse_unary();
        return parse_primary();
    }

    ExprPtr parse_primary() {
        const Token& t = peek();
        switch (t.kind) {
            case TokenKind::Integer: return Expr::make_literal(Value::integer(parse_int(advance().text)));
            case TokenKind::Float: return Expr::make_literal(Value::real(std::strtod(advance().text.c_str(), nullptr)));
            case TokenKind::String: return Expr::make_literal(Value::text(advance().text));
            case TokenKind::Parameter: {
                const Token& p = advance();
                auto e = std::make_unique<Expr>();
                e->kind = ExprKind::Parameter;
                if (p.text.empty()) {
                    if (saw_numbered_) fail("cannot mix '?' and '$n' parameters");
                    saw_positional_ = true;
                    e->slot = next_positional_++;
                } else {
                    if (saw_positional_) fail("cannot mix '?' and '$n' parameters");
                    saw_numbered_ = true;
                    e->slot = std::stoi(p.text) - 1;
                }
                return e;
            }
            case TokenKind::Symbol:
                if (accept_symbol("(")) {
                    ExprPtr e = parse_expr();
                    expect_symbol(")");
                    return e;
                }
                fail("expected expression");
            case TokenKind::End: fail("expected expression");
            default: break;
        }

        if (accept_kw("NULL")) return Expr::make_literal(Value::null());
        if (accept_kw("TRUE")) return Expr::make_literal(Value::boolean(true));
        if (accept_kw("FALSE")) return Expr::make_literal(Value::boolean(false));

        // Function call: name '(' ... ')'. Function names may shadow reserved words (e.g. LEFT).
        if (t.kind == TokenKind::Identifier && is_symbol("(", 1)) {
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Function;
            e->name = upper(advance().text);
            advance();  // (
            if (accept_symbol("*")) {
                if (e->name != "COUNT") fail("'*' argument is only valid for COUNT");
                e->star = true;
            } else if (!is_symbol(")")) {
                if (accept_kw("DISTINCT")) {
                    if (!is_aggregate_function(e->name)) fail("DISTINCT is only valid in aggregate functions");
                    e->distinct = true;
                }
                do e->args.push_back(parse_expr());
                while (accept_symbol(","));
            }
            expect_symbol(")");
            return e;
        }

        std::string first = identifier("expression");
        if (accept_symbol(".")) return Expr::make_column(first, identifier("column name"));
        return Expr::make_column("", first);
    }

    std::vector<Token> toks_;
    size_t pos_ = 0;
    int next_positional_ = 0;
    bool saw_positional_ = false;
    bool saw_numbered_ = false;
};

}  // namespace

std::vector<Statement> parse_sql(std::string_view sql) { return Parser(sql).parse_script(); }

Statement parse_statement(std::string_view sql) { return Parser(sql).parse_single(); }

ExprPtr parse_expression(std::string_view sql) { return Parser(sql).parse_standalone_expr(); }

}  // namespace atlas
