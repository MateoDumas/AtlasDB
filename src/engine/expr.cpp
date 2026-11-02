#include "engine/expr.h"

#include <cctype>
#include <cmath>
#include <limits>

#include "common/error.h"

namespace atlas {

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

void Scope::add_table(const std::string& alias, const TableSchema& schema) {
    for (const auto& c : schema.columns) cols_.push_back({alias, c.name, c.type});
}

bool Scope::has_table(const std::string& alias) const {
    for (const auto& c : cols_)
        if (c.table == alias) return true;
    return false;
}

int Scope::resolve(const std::string& table, const std::string& name) const {
    int found = -1;
    for (size_t i = 0; i < cols_.size(); ++i) {
        if (cols_[i].name != name) continue;
        if (!table.empty() && cols_[i].table != table) continue;
        if (found >= 0) throw DbError(ErrorCode::Semantic, "column reference '" + name + "' is ambiguous");
        found = static_cast<int>(i);
    }
    if (found < 0) {
        if (!table.empty() && !has_table(table))
            throw DbError(ErrorCode::Semantic, "unknown table or alias '" + table + "'");
        throw DbError(ErrorCode::Semantic,
                      "unknown column '" + (table.empty() ? name : table + "." + name) + "'");
    }
    return found;
}

void bind_expr(Expr& e, const Scope& scope) {
    if (e.kind == ExprKind::Column) e.slot = scope.resolve(e.table, e.name);
    for (auto& a : e.args) bind_expr(*a, scope);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

[[noreturn]] void type_error(const std::string& msg) { throw DbError(ErrorCode::Type, msg); }

int64_t checked(BinaryOp op, int64_t a, int64_t b) {
    int64_t r = 0;
    bool overflow = false;
#if defined(__GNUC__) || defined(__clang__)
    switch (op) {
        case BinaryOp::Add: overflow = __builtin_add_overflow(a, b, &r); break;
        case BinaryOp::Sub: overflow = __builtin_sub_overflow(a, b, &r); break;
        case BinaryOp::Mul: overflow = __builtin_mul_overflow(a, b, &r); break;
        default: break;
    }
#else
    long double x = static_cast<long double>(a), y = static_cast<long double>(b), z = 0;
    switch (op) {
        case BinaryOp::Add: z = x + y; r = a + b; break;
        case BinaryOp::Sub: z = x - y; r = a - b; break;
        case BinaryOp::Mul: z = x * y; r = a * b; break;
        default: break;
    }
    overflow = z > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
               z < static_cast<long double>(std::numeric_limits<int64_t>::min());
#endif
    if (overflow) throw DbError(ErrorCode::Type, "integer overflow");
    return r;
}

Value arithmetic(BinaryOp op, const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return Value::null();
    if (!a.is_numeric() || !b.is_numeric())
        type_error("operator " + std::string(binary_op_symbol(op)) + " requires numbers, got " +
                   std::string(type_name(a.type())) + " and " + std::string(type_name(b.type())));
    if (a.type() == DataType::Integer && b.type() == DataType::Integer) {
        int64_t x = a.as_int(), y = b.as_int();
        switch (op) {
            case BinaryOp::Add:
            case BinaryOp::Sub:
            case BinaryOp::Mul: return Value::integer(checked(op, x, y));
            case BinaryOp::Div:
                if (y == 0) throw DbError(ErrorCode::Type, "division by zero");
                if (x == std::numeric_limits<int64_t>::min() && y == -1) throw DbError(ErrorCode::Type, "integer overflow");
                return Value::integer(x / y);
            case BinaryOp::Mod:
                if (y == 0) throw DbError(ErrorCode::Type, "division by zero");
                if (y == -1) return Value::integer(0);
                return Value::integer(x % y);
            default: break;
        }
    }
    double x = a.as_real(), y = b.as_real();
    switch (op) {
        case BinaryOp::Add: return Value::real(x + y);
        case BinaryOp::Sub: return Value::real(x - y);
        case BinaryOp::Mul: return Value::real(x * y);
        case BinaryOp::Div:
            if (y == 0) throw DbError(ErrorCode::Type, "division by zero");
            return Value::real(x / y);
        case BinaryOp::Mod:
            if (y == 0) throw DbError(ErrorCode::Type, "division by zero");
            return Value::real(std::fmod(x, y));
        default: break;
    }
    throw DbError(ErrorCode::Internal, "bad arithmetic operator");
}

bool comparable(const Value& a, const Value& b) {
    if (a.is_numeric() && b.is_numeric()) return true;
    return a.type() == b.type();
}

// Three-valued comparison; returns NULL if either side is NULL.
Value compare_op(BinaryOp op, const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return Value::null();
    if (!comparable(a, b))
        type_error("cannot compare " + std::string(type_name(a.type())) + " with " +
                   std::string(type_name(b.type())));
    int c = compare_values(a, b);
    bool r = false;
    switch (op) {
        case BinaryOp::Eq: r = c == 0; break;
        case BinaryOp::Ne: r = c != 0; break;
        case BinaryOp::Lt: r = c < 0; break;
        case BinaryOp::Le: r = c <= 0; break;
        case BinaryOp::Gt: r = c > 0; break;
        case BinaryOp::Ge: r = c >= 0; break;
        default: break;
    }
    return Value::boolean(r);
}

// Returns -1 for NULL, 0 for false, 1 for true.
int truth(const Value& v) {
    if (v.is_null()) return -1;
    if (v.type() != DataType::Boolean)
        type_error("expected a BOOLEAN condition, got " + std::string(type_name(v.type())));
    return v.as_bool() ? 1 : 0;
}

Value from_truth(int t) { return t < 0 ? Value::null() : Value::boolean(t == 1); }

std::string case_map(std::string s, bool up) {
    for (auto& c : s)
        c = static_cast<char>(up ? std::toupper(static_cast<unsigned char>(c)) : std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void expect_args(const Expr& e, size_t lo, size_t hi) {
    if (e.args.size() < lo || e.args.size() > hi)
        throw DbError(ErrorCode::Semantic, "wrong number of arguments to " + e.name);
}

Value call_function(const Expr& e, const Row& row) {
    const std::string& f = e.name;
    if (is_aggregate_function(f))
        throw DbError(ErrorCode::Semantic, "aggregate function " + f + " is not allowed here");

    if (f == "COALESCE" || f == "IFNULL") {
        if (e.args.empty()) throw DbError(ErrorCode::Semantic, f + " requires arguments");
        for (const auto& a : e.args) {
            Value v = eval_expr(*a, row);
            if (!v.is_null()) return v;
        }
        return Value::null();
    }

    std::vector<Value> args;
    for (const auto& a : e.args) args.push_back(eval_expr(*a, row));

    if (f == "UPPER" || f == "LOWER") {
        expect_args(e, 1, 1);
        if (args[0].is_null()) return args[0];
        return Value::text(case_map(args[0].as_text(), f == "UPPER"));
    }
    if (f == "LENGTH") {
        expect_args(e, 1, 1);
        if (args[0].is_null()) return args[0];
        const std::string& s = args[0].as_text();
        int64_t n = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) ++n;
        return Value::integer(n);
    }
    if (f == "ABS") {
        expect_args(e, 1, 1);
        if (args[0].is_null()) return args[0];
        if (args[0].type() == DataType::Integer) {
            int64_t v = args[0].as_int();
            if (v == std::numeric_limits<int64_t>::min()) throw DbError(ErrorCode::Type, "integer overflow");
            return Value::integer(v < 0 ? -v : v);
        }
        return Value::real(std::fabs(args[0].as_real()));
    }
    if (f == "ROUND") {
        expect_args(e, 1, 2);
        if (args[0].is_null()) return args[0];
        int64_t digits = args.size() > 1 && !args[1].is_null() ? args[1].as_int() : 0;
        if (args[0].type() == DataType::Integer && digits >= 0) return args[0];
        double scale = std::pow(10.0, static_cast<double>(digits));
        return Value::real(std::round(args[0].as_real() * scale) / scale);
    }
    if (f == "SUBSTR" || f == "SUBSTRING") {
        expect_args(e, 2, 3);
        for (const auto& a : args)
            if (a.is_null()) return Value::null();
        const std::string& s = args[0].as_text();
        int64_t start = std::max<int64_t>(args[1].as_int(), 1) - 1;  // 1-based
        if (start >= static_cast<int64_t>(s.size())) return Value::text("");
        size_t len = args.size() > 2 ? static_cast<size_t>(std::max<int64_t>(args[2].as_int(), 0)) : std::string::npos;
        return Value::text(s.substr(static_cast<size_t>(start), len));
    }
    if (f == "TYPEOF") {
        expect_args(e, 1, 1);
        return Value::text(std::string(type_name(args[0].type())));
    }
    throw DbError(ErrorCode::Semantic, "unknown function " + f);
}

}  // namespace

bool like_match(std::string_view s, std::string_view p) {
    // Iterative wildcard matching with backtracking on the last '%'.
    size_t si = 0, pi = 0, star_p = std::string_view::npos, star_s = 0;
    while (si < s.size()) {
        if (pi < p.size() && (p[pi] == '_' || p[pi] == s[si])) {
            ++si;
            ++pi;
        } else if (pi < p.size() && p[pi] == '%') {
            star_p = pi++;
            star_s = si;
        } else if (star_p != std::string_view::npos) {
            pi = star_p + 1;
            si = ++star_s;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '%') ++pi;
    return pi == p.size();
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

Value eval_expr(const Expr& e, const Row& row) {
    switch (e.kind) {
        case ExprKind::Literal: return e.literal;
        case ExprKind::Column:
        case ExprKind::Slot:
            if (e.slot < 0 || static_cast<size_t>(e.slot) >= row.size())
                throw DbError(ErrorCode::Internal, "unbound column reference " + e.to_string());
            return row[static_cast<size_t>(e.slot)];
        case ExprKind::Unary: {
            Value v = eval_expr(*e.args[0], row);
            if (e.uop == UnaryOp::Not) {
                int t = truth(v);
                return t < 0 ? v : Value::boolean(t == 0);
            }
            if (v.is_null()) return v;
            if (v.type() == DataType::Integer) {
                if (v.as_int() == std::numeric_limits<int64_t>::min()) throw DbError(ErrorCode::Type, "integer overflow");
                return Value::integer(-v.as_int());
            }
            if (v.type() == DataType::Real) return Value::real(-v.as_real());
            type_error("cannot negate " + std::string(type_name(v.type())));
        }
        case ExprKind::Binary: {
            if (e.bop == BinaryOp::And || e.bop == BinaryOp::Or) {
                int l = truth(eval_expr(*e.args[0], row));
                if (e.bop == BinaryOp::And && l == 0) return Value::boolean(false);
                if (e.bop == BinaryOp::Or && l == 1) return Value::boolean(true);
                int r = truth(eval_expr(*e.args[1], row));
                if (e.bop == BinaryOp::And) {
                    if (r == 0) return Value::boolean(false);
                    return from_truth(l < 0 || r < 0 ? -1 : 1);
                }
                if (r == 1) return Value::boolean(true);
                return from_truth(l < 0 || r < 0 ? -1 : 0);
            }
            Value a = eval_expr(*e.args[0], row);
            Value b = eval_expr(*e.args[1], row);
            switch (e.bop) {
                case BinaryOp::Add:
                case BinaryOp::Sub:
                case BinaryOp::Mul:
                case BinaryOp::Div:
                case BinaryOp::Mod: return arithmetic(e.bop, a, b);
                case BinaryOp::Concat:
                    if (a.is_null() || b.is_null()) return Value::null();
                    return Value::text(a.to_string() + b.to_string());
                default: return compare_op(e.bop, a, b);
            }
        }
        case ExprKind::IsNull: {
            bool n = eval_expr(*e.args[0], row).is_null();
            return Value::boolean(e.negated ? !n : n);
        }
        case ExprKind::In: {
            Value needle = eval_expr(*e.args[0], row);
            if (needle.is_null()) return needle;
            bool saw_null = false;
            for (size_t i = 1; i < e.args.size(); ++i) {
                Value v = eval_expr(*e.args[i], row);
                Value eq = compare_op(BinaryOp::Eq, needle, v);
                if (eq.is_null()) saw_null = true;
                else if (eq.as_bool()) return Value::boolean(!e.negated);
            }
            if (saw_null) return Value::null();
            return Value::boolean(e.negated);
        }
        case ExprKind::Between: {
            Value v = eval_expr(*e.args[0], row);
            int lo = truth(compare_op(BinaryOp::Ge, v, eval_expr(*e.args[1], row)));
            int hi = truth(compare_op(BinaryOp::Le, v, eval_expr(*e.args[2], row)));
            int r = (lo == 0 || hi == 0) ? 0 : (lo < 0 || hi < 0 ? -1 : 1);
            if (e.negated && r >= 0) r = 1 - r;
            return from_truth(r);
        }
        case ExprKind::Like: {
            Value s = eval_expr(*e.args[0], row);
            Value p = eval_expr(*e.args[1], row);
            if (s.is_null() || p.is_null()) return Value::null();
            if (s.type() != DataType::Text || p.type() != DataType::Text) type_error("LIKE requires TEXT operands");
            bool m = like_match(s.as_text(), p.as_text());
            return Value::boolean(e.negated ? !m : m);
        }
        case ExprKind::Function: return call_function(e, row);
        case ExprKind::Parameter:
            throw DbError(ErrorCode::Semantic, "no value supplied for parameter " + e.to_string());
    }
    throw DbError(ErrorCode::Internal, "unknown expression kind");
}

bool eval_predicate(const Expr& e, const Row& row) { return truth(eval_expr(e, row)) == 1; }

bool contains_aggregate(const Expr& e) {
    if (e.kind == ExprKind::Function && is_aggregate_function(e.name)) return true;
    for (const auto& a : e.args)
        if (contains_aggregate(*a)) return true;
    return false;
}

bool contains_columns(const Expr& e) {
    if (e.kind == ExprKind::Column || e.kind == ExprKind::Slot) return true;
    for (const auto& a : e.args)
        if (contains_columns(*a)) return true;
    return false;
}

void collect_slots(const Expr& e, std::vector<int>& out) {
    if ((e.kind == ExprKind::Column || e.kind == ExprKind::Slot) && e.slot >= 0) out.push_back(e.slot);
    for (const auto& a : e.args) collect_slots(*a, out);
}

DataType infer_type(const Expr& e, const Scope& scope) {
    switch (e.kind) {
        case ExprKind::Literal: return e.literal.type();
        case ExprKind::Parameter: return DataType::Null;
        case ExprKind::Column:
        case ExprKind::Slot:
            if (e.slot >= 0 && static_cast<size_t>(e.slot) < scope.size())
                return scope.columns()[static_cast<size_t>(e.slot)].type;
            return DataType::Null;
        case ExprKind::Unary:
            return e.uop == UnaryOp::Not ? DataType::Boolean : infer_type(*e.args[0], scope);
        case ExprKind::Binary:
            switch (e.bop) {
                case BinaryOp::Add:
                case BinaryOp::Sub:
                case BinaryOp::Mul:
                case BinaryOp::Div:
                case BinaryOp::Mod: {
                    DataType a = infer_type(*e.args[0], scope), b = infer_type(*e.args[1], scope);
                    if (a == DataType::Real || b == DataType::Real) return DataType::Real;
                    if (a == DataType::Integer && b == DataType::Integer) return DataType::Integer;
                    return a == DataType::Null ? b : a;
                }
                case BinaryOp::Concat: return DataType::Text;
                default: return DataType::Boolean;
            }
        case ExprKind::IsNull:
        case ExprKind::In:
        case ExprKind::Between:
        case ExprKind::Like: return DataType::Boolean;
        case ExprKind::Function: {
            const std::string& f = e.name;
            if (f == "COUNT" || f == "LENGTH") return DataType::Integer;
            if (f == "AVG") return DataType::Real;
            if (f == "UPPER" || f == "LOWER" || f == "SUBSTR" || f == "SUBSTRING" || f == "TYPEOF")
                return DataType::Text;
            if (f == "ROUND") return e.args.size() == 1 ? infer_type(*e.args[0], scope) : DataType::Real;
            if (!e.args.empty()) return infer_type(*e.args[0], scope);
            return DataType::Null;
        }
    }
    return DataType::Null;
}

void split_conjuncts(ExprPtr e, std::vector<ExprPtr>& out) {
    if (!e) return;
    if (e->kind == ExprKind::Binary && e->bop == BinaryOp::And) {
        split_conjuncts(std::move(e->args[0]), out);
        split_conjuncts(std::move(e->args[1]), out);
        return;
    }
    out.push_back(std::move(e));
}

ExprPtr join_conjuncts(std::vector<ExprPtr> terms) {
    ExprPtr result;
    for (auto& t : terms) {
        if (!t) continue;
        result = result ? Expr::make_binary(BinaryOp::And, std::move(result), std::move(t)) : std::move(t);
    }
    return result;
}

}  // namespace atlas
