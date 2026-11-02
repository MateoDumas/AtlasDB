#include "sql/ast.h"

#include <algorithm>
#include <functional>

#include "common/error.h"

namespace atlas {

ExprPtr Expr::make_literal(Value v) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Literal;
    e->literal = std::move(v);
    return e;
}

ExprPtr Expr::make_column(std::string table, std::string name) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Column;
    e->table = std::move(table);
    e->name = std::move(name);
    return e;
}

ExprPtr Expr::make_slot(int slot, std::string display) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Slot;
    e->slot = slot;
    e->name = std::move(display);
    return e;
}

ExprPtr Expr::make_unary(UnaryOp op, ExprPtr a) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Unary;
    e->uop = op;
    e->args.push_back(std::move(a));
    return e;
}

ExprPtr Expr::make_binary(BinaryOp op, ExprPtr a, ExprPtr b) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Binary;
    e->bop = op;
    e->args.push_back(std::move(a));
    e->args.push_back(std::move(b));
    return e;
}

ExprPtr Expr::clone() const {
    auto e = std::make_unique<Expr>();
    e->kind = kind;
    e->literal = literal;
    e->table = table;
    e->name = name;
    e->uop = uop;
    e->bop = bop;
    e->negated = negated;
    e->distinct = distinct;
    e->star = star;
    e->slot = slot;
    for (const auto& a : args) e->args.push_back(a->clone());
    return e;
}

std::string_view binary_op_symbol(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::Concat: return "||";
        case BinaryOp::Eq: return "=";
        case BinaryOp::Ne: return "!=";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Ge: return ">=";
        case BinaryOp::And: return "AND";
        case BinaryOp::Or: return "OR";
    }
    return "?";
}

bool is_aggregate_function(const std::string& n) {
    return n == "COUNT" || n == "SUM" || n == "AVG" || n == "MIN" || n == "MAX";
}

std::string Expr::to_string() const {
    switch (kind) {
        case ExprKind::Literal: return literal.to_sql();
        case ExprKind::Column: return table.empty() ? name : table + "." + name;
        case ExprKind::Slot: return name.empty() ? "#" + std::to_string(slot) : name;
        case ExprKind::Parameter: return "$" + std::to_string(slot + 1);
        case ExprKind::Unary:
            return uop == UnaryOp::Neg ? "-" + args[0]->to_string() : "NOT " + args[0]->to_string();
        case ExprKind::Binary:
            return "(" + args[0]->to_string() + " " + std::string(binary_op_symbol(bop)) + " " +
                   args[1]->to_string() + ")";
        case ExprKind::IsNull:
            return args[0]->to_string() + (negated ? " IS NOT NULL" : " IS NULL");
        case ExprKind::In: {
            std::string s = args[0]->to_string() + (negated ? " NOT IN (" : " IN (");
            for (size_t i = 1; i < args.size(); ++i) {
                if (i > 1) s += ", ";
                s += args[i]->to_string();
            }
            return s + ")";
        }
        case ExprKind::Between:
            return args[0]->to_string() + (negated ? " NOT BETWEEN " : " BETWEEN ") + args[1]->to_string() +
                   " AND " + args[2]->to_string();
        case ExprKind::Like:
            return args[0]->to_string() + (negated ? " NOT LIKE " : " LIKE ") + args[1]->to_string();
        case ExprKind::Function: {
            std::string s = name + "(";
            if (star) return s + "*)";
            if (distinct) s += "DISTINCT ";
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) s += ", ";
                s += args[i]->to_string();
            }
            return s + ")";
        }
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Statement utilities
// ---------------------------------------------------------------------------

namespace {

ExprPtr clone_expr(const ExprPtr& e) { return e ? e->clone() : nullptr; }

std::vector<ExprPtr> clone_exprs(const std::vector<ExprPtr>& v) {
    std::vector<ExprPtr> out;
    out.reserve(v.size());
    for (const auto& e : v) out.push_back(clone_expr(e));
    return out;
}

SelectStmt clone_select(const SelectStmt& s) {
    SelectStmt c;
    c.distinct = s.distinct;
    for (const auto& item : s.items) c.items.push_back({clone_expr(item.expr), item.alias, item.star, item.star_table});
    c.from = s.from;
    for (const auto& j : s.joins) c.joins.push_back({j.type, j.table, clone_expr(j.on)});
    c.where = clone_expr(s.where);
    c.group_by = clone_exprs(s.group_by);
    c.having = clone_expr(s.having);
    for (const auto& o : s.order_by) c.order_by.push_back({clone_expr(o.expr), o.desc});
    c.limit = clone_expr(s.limit);
    c.offset = clone_expr(s.offset);
    return c;
}

struct Cloner {
    Statement operator()(const CreateTableStmt& s) const {
        CreateTableStmt c;
        c.name = s.name;
        c.if_not_exists = s.if_not_exists;
        for (const auto& col : s.columns) {
            ColumnDefAst d;
            d.name = col.name;
            d.type = col.type;
            d.max_length = col.max_length;
            d.not_null = col.not_null;
            d.primary_key = col.primary_key;
            d.unique = col.unique;
            d.default_value = clone_expr(col.default_value);
            d.check = clone_expr(col.check);
            d.references = col.references;
            c.columns.push_back(std::move(d));
        }
        c.primary_key = s.primary_key;
        c.unique = s.unique;
        c.foreign_keys = s.foreign_keys;
        c.checks = clone_exprs(s.checks);
        return {std::move(c)};
    }
    Statement operator()(const InsertStmt& s) const {
        InsertStmt c;
        c.table = s.table;
        c.columns = s.columns;
        for (const auto& row : s.rows) c.rows.push_back(clone_exprs(row));
        if (s.select) c.select = std::make_unique<SelectStmt>(clone_select(*s.select));
        return {std::move(c)};
    }
    Statement operator()(const SelectStmt& s) const { return {clone_select(s)}; }
    Statement operator()(const UpdateStmt& s) const {
        UpdateStmt c;
        c.table = s.table;
        for (const auto& [col, e] : s.assignments) c.assignments.emplace_back(col, clone_expr(e));
        c.where = clone_expr(s.where);
        return {std::move(c)};
    }
    Statement operator()(const DeleteStmt& s) const { return {DeleteStmt{s.table, clone_expr(s.where)}}; }
    Statement operator()(const ExplainStmt& s) const {
        ExplainStmt c;
        c.inner = std::make_unique<Statement>(clone_statement(*s.inner));
        return {std::move(c)};
    }
    // Statements without expressions are plain copyable values.
    template <class T>
    Statement operator()(const T& s) const {
        return {s};
    }
};

using ExprVisitor = std::function<void(ExprPtr&)>;

void visit_select(SelectStmt& s, const ExprVisitor& f) {
    for (auto& item : s.items) f(item.expr);
    for (auto& j : s.joins) f(j.on);
    f(s.where);
    for (auto& g : s.group_by) f(g);
    f(s.having);
    for (auto& o : s.order_by) f(o.expr);
    f(s.limit);
    f(s.offset);
}

// Calls f on every top-level expression slot of a statement (slots may be null).
void visit_exprs(Statement& st, const ExprVisitor& f) {
    if (auto* sel = std::get_if<SelectStmt>(&st.node)) {
        visit_select(*sel, f);
    } else if (auto* ins = std::get_if<InsertStmt>(&st.node)) {
        for (auto& row : ins->rows)
            for (auto& e : row) f(e);
        if (ins->select) visit_select(*ins->select, f);
    } else if (auto* upd = std::get_if<UpdateStmt>(&st.node)) {
        for (auto& [col, e] : upd->assignments) f(e);
        f(upd->where);
    } else if (auto* del = std::get_if<DeleteStmt>(&st.node)) {
        f(del->where);
    } else if (auto* ct = std::get_if<CreateTableStmt>(&st.node)) {
        for (auto& c : ct->columns) {
            f(c.default_value);
            f(c.check);
        }
        for (auto& c : ct->checks) f(c);
    } else if (auto* ex = std::get_if<ExplainStmt>(&st.node)) {
        visit_exprs(*ex->inner, f);
    }
}

void max_parameter(const Expr& e, size_t& n) {
    if (e.kind == ExprKind::Parameter) n = std::max(n, static_cast<size_t>(e.slot) + 1);
    for (const auto& a : e.args) max_parameter(*a, n);
}

void substitute(ExprPtr& e, const std::vector<Value>& values) {
    if (!e) return;
    if (e->kind == ExprKind::Parameter) {
        e = Expr::make_literal(values[static_cast<size_t>(e->slot)]);
        return;
    }
    for (auto& a : e->args) substitute(a, values);
}

}  // namespace

Statement clone_statement(const Statement& s) { return std::visit(Cloner{}, s.node); }

size_t count_parameters(const Statement& s) {
    size_t n = 0;
    // visit_exprs only reads here; the const_cast avoids a duplicate const visitor.
    visit_exprs(const_cast<Statement&>(s), [&](ExprPtr& e) {
        if (e) max_parameter(*e, n);
    });
    return n;
}

void bind_parameters(Statement& s, const std::vector<Value>& values) {
    size_t needed = count_parameters(s);
    if (values.size() != needed)
        throw DbError(ErrorCode::Semantic, "statement expects " + std::to_string(needed) + " parameter(s), got " +
                                               std::to_string(values.size()));
    if (needed == 0) return;
    visit_exprs(s, [&](ExprPtr& e) { substitute(e, values); });
}

}  // namespace atlas
