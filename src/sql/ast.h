#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/value.h"

namespace atlas {

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

enum class ExprKind {
    Literal,   // literal value
    Column,    // [table.]name; `slot` is filled in by binding
    Slot,      // direct reference to position `slot` of the input row (planner generated)
    Unary,     // op args[0]
    Binary,    // args[0] op args[1]
    IsNull,    // args[0] IS [NOT] NULL
    In,        // args[0] [NOT] IN (args[1..])
    Between,   // args[0] [NOT] BETWEEN args[1] AND args[2]
    Like,      // args[0] [NOT] LIKE args[1]
    Function,  // name(args...), also aggregates; `star` for COUNT(*)
    Parameter, // $n / ? placeholder; `slot` is the 0-based parameter index
};

enum class UnaryOp { Neg, Not };

enum class BinaryOp { Add, Sub, Mul, Div, Mod, Concat, Eq, Ne, Lt, Le, Gt, Ge, And, Or };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    ExprKind kind = ExprKind::Literal;
    Value literal;
    std::string table;  // column qualifier
    std::string name;   // column name, or upper-cased function name
    UnaryOp uop = UnaryOp::Neg;
    BinaryOp bop = BinaryOp::Add;
    bool negated = false;   // NOT IN / NOT LIKE / NOT BETWEEN / IS NOT NULL
    bool distinct = false;  // COUNT(DISTINCT x)
    bool star = false;      // COUNT(*)
    std::vector<ExprPtr> args;
    int slot = -1;

    ExprPtr clone() const;
    std::string to_string() const;

    static ExprPtr make_literal(Value v);
    static ExprPtr make_column(std::string table, std::string name);
    static ExprPtr make_slot(int slot, std::string display);
    static ExprPtr make_unary(UnaryOp op, ExprPtr a);
    static ExprPtr make_binary(BinaryOp op, ExprPtr a, ExprPtr b);
};

std::string_view binary_op_symbol(BinaryOp op);
bool is_aggregate_function(const std::string& upper_name);

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

struct ForeignKeyRef {
    std::string table;
    std::string column;
};

struct ColumnDefAst {
    std::string name;
    DataType type = DataType::Text;
    uint32_t max_length = 0;  // VARCHAR(n); 0 = unlimited
    bool not_null = false;
    bool primary_key = false;
    bool unique = false;
    ExprPtr default_value;
    ExprPtr check;
    std::optional<ForeignKeyRef> references;
};

struct CreateTableStmt {
    std::string name;
    bool if_not_exists = false;
    std::vector<ColumnDefAst> columns;
    std::vector<std::string> primary_key;  // table-level PRIMARY KEY (col)
    std::vector<std::string> unique;       // table-level UNIQUE (col)
    std::vector<std::pair<std::string, ForeignKeyRef>> foreign_keys;
    std::vector<ExprPtr> checks;
};

struct DropTableStmt {
    std::string name;
    bool if_exists = false;
};

struct CreateIndexStmt {
    std::string name;
    std::string table;
    std::string column;
    bool unique = false;
    bool if_not_exists = false;
};

struct DropIndexStmt {
    std::string name;
    bool if_exists = false;
};

struct SelectStmt;

struct InsertStmt {
    std::string table;
    std::vector<std::string> columns;
    std::vector<std::vector<ExprPtr>> rows;
    std::unique_ptr<SelectStmt> select;  // INSERT ... SELECT
};

enum class JoinType { Inner, Left, Cross };

struct TableRef {
    std::string name;
    std::string alias;  // empty when none
    const std::string& effective_name() const { return alias.empty() ? name : alias; }
};

struct JoinClause {
    JoinType type = JoinType::Inner;
    TableRef table;
    ExprPtr on;
};

struct SelectItem {
    ExprPtr expr;          // null when star
    std::string alias;
    bool star = false;
    std::string star_table;  // for t.*
};

struct OrderItem {
    ExprPtr expr;
    bool desc = false;
};

struct SelectStmt {
    bool distinct = false;
    std::vector<SelectItem> items;
    std::optional<TableRef> from;
    std::vector<JoinClause> joins;
    ExprPtr where;
    std::vector<ExprPtr> group_by;
    ExprPtr having;
    std::vector<OrderItem> order_by;
    ExprPtr limit;
    ExprPtr offset;
};

struct UpdateStmt {
    std::string table;
    std::vector<std::pair<std::string, ExprPtr>> assignments;
    ExprPtr where;
};

struct DeleteStmt {
    std::string table;
    ExprPtr where;
};

struct BeginStmt {};
struct CommitStmt {};
struct RollbackStmt {};
struct CheckpointStmt {};
struct ShowTablesStmt {};
struct DescribeStmt {
    std::string table;
};

struct Statement;

struct ExplainStmt {
    std::unique_ptr<Statement> inner;
};

struct Statement {
    std::variant<CreateTableStmt, DropTableStmt, CreateIndexStmt, DropIndexStmt, InsertStmt, SelectStmt,
                 UpdateStmt, DeleteStmt, BeginStmt, CommitStmt, RollbackStmt, CheckpointStmt, ShowTablesStmt,
                 DescribeStmt, ExplainStmt>
        node;
};

// Deep copy (execution consumes parts of a statement, so prepared statements
// keep a pristine copy and clone it for every run).
Statement clone_statement(const Statement& s);

// Number of parameters a statement expects: the highest $n / ? index used.
size_t count_parameters(const Statement& s);

// Replaces every parameter placeholder with the corresponding literal value.
// Throws DbError(Semantic) if the number of values does not match.
void bind_parameters(Statement& s, const std::vector<Value>& values);

}  // namespace atlas
