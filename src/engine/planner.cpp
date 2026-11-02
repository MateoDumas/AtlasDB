#include "engine/planner.h"

#include <algorithm>
#include <map>
#include <set>

#include "common/error.h"
#include "storage/row_codec.h"

namespace atlas {

namespace {

[[noreturn]] void semantic(const std::string& msg) { throw DbError(ErrorCode::Semantic, msg); }

bool is_constant(const Expr& e) { return !contains_columns(e) && !contains_aggregate(e); }

std::optional<Value> constant_value(const Expr& e) {
    if (!is_constant(e)) return std::nullopt;
    try {
        return eval_expr(e, {});
    } catch (const DbError&) {
        return std::nullopt;
    }
}

bool index_compatible(DataType column, const Value& v) {
    if (v.is_null()) return false;
    bool num_col = column == DataType::Integer || column == DataType::Real;
    return num_col ? v.is_numeric() : v.type() == column;
}

BinaryOp flip(BinaryOp op) {
    switch (op) {
        case BinaryOp::Lt: return BinaryOp::Gt;
        case BinaryOp::Le: return BinaryOp::Ge;
        case BinaryOp::Gt: return BinaryOp::Lt;
        case BinaryOp::Ge: return BinaryOp::Le;
        default: return op;
    }
}

bool is_comparison(BinaryOp op) {
    return op == BinaryOp::Eq || op == BinaryOp::Lt || op == BinaryOp::Le || op == BinaryOp::Gt || op == BinaryOp::Ge;
}

struct Candidate {
    size_t pred;
    int column;
    BinaryOp op;
    Value value;
};

// Picks an index access path for one table from predicates bound to that
// table's row layout. Predicates fully answered by the index are removed.
AccessPath choose_access_path(Table& t, std::vector<ExprPtr>& preds) {
    const auto& cols = t.schema().columns;
    std::vector<Candidate> cands;
    std::vector<std::pair<size_t, std::vector<Value>>> in_lists;  // (pred, values) with column in pred

    auto usable = [&](const Expr& col, const Value& v) {
        return col.kind == ExprKind::Column && col.slot >= 0 && t.index_on(col.slot) &&
               index_compatible(cols[static_cast<size_t>(col.slot)].type, v);
    };

    for (size_t i = 0; i < preds.size(); ++i) {
        const Expr& p = *preds[i];
        if (p.kind == ExprKind::Binary && is_comparison(p.bop)) {
            const Expr& a = *p.args[0];
            const Expr& b = *p.args[1];
            if (a.kind == ExprKind::Column) {
                if (auto v = constant_value(b); v && usable(a, *v)) cands.push_back({i, a.slot, p.bop, *v});
            } else if (b.kind == ExprKind::Column) {
                if (auto v = constant_value(a); v && usable(b, *v)) cands.push_back({i, b.slot, flip(p.bop), *v});
            }
        } else if (p.kind == ExprKind::Between && !p.negated && p.args[0]->kind == ExprKind::Column) {
            auto lo = constant_value(*p.args[1]);
            auto hi = constant_value(*p.args[2]);
            if (lo && hi && usable(*p.args[0], *lo) && usable(*p.args[0], *hi)) {
                cands.push_back({i, p.args[0]->slot, BinaryOp::Ge, *lo});
                cands.push_back({i, p.args[0]->slot, BinaryOp::Le, *hi});
            }
        } else if (p.kind == ExprKind::In && !p.negated && p.args[0]->kind == ExprKind::Column) {
            std::vector<Value> values;
            bool ok = true;
            for (size_t k = 1; k < p.args.size() && ok; ++k) {
                auto v = constant_value(*p.args[k]);
                if (!v) ok = false;
                else if (v->is_null()) continue;  // NULL never matches
                else if (!usable(*p.args[0], *v)) ok = false;
                else values.push_back(*v);
            }
            if (ok && t.index_on(p.args[0]->slot)) in_lists.emplace_back(i, std::move(values));
        }
    }

    AccessPath path;
    std::set<size_t> consumed;

    // 1. Equality, preferring unique indexes.
    const Candidate* best_eq = nullptr;
    for (const auto& c : cands) {
        if (c.op != BinaryOp::Eq) continue;
        if (!best_eq || (t.index_on(c.column)->def().unique && !t.index_on(best_eq->column)->def().unique))
            best_eq = &c;
    }
    if (best_eq) {
        path.index = t.index_on(best_eq->column);
        path.eq_values = {best_eq->value};
        consumed.insert(best_eq->pred);
    } else if (!in_lists.empty()) {
        // 2. IN list: several point lookups.
        auto& [pred, values] = in_lists.front();
        path.index = t.index_on(preds[pred]->args[0]->slot);
        path.eq_values = values;
        if (path.eq_values.empty()) path.eq_values.push_back(Value::null());  // matches nothing
        consumed.insert(pred);
    } else if (!cands.empty()) {
        // 3. Range on the column with the most bounds.
        std::map<int, int> per_column;
        for (const auto& c : cands) ++per_column[c.column];
        int column = std::max_element(per_column.begin(), per_column.end(),
                                      [](auto& a, auto& b) { return a.second < b.second; })
                         ->first;
        path.index = t.index_on(column);
        for (const auto& c : cands) {
            if (c.column != column) continue;
            bool inclusive = c.op == BinaryOp::Ge || c.op == BinaryOp::Le;
            if (c.op == BinaryOp::Gt || c.op == BinaryOp::Ge) {
                int cmp = path.lo ? compare_values(c.value, path.lo->value) : 1;
                if (cmp > 0 || (cmp == 0 && !inclusive)) path.lo = IndexBound{c.value, inclusive};
            } else {
                int cmp = path.hi ? compare_values(c.value, path.hi->value) : -1;
                if (cmp < 0 || (cmp == 0 && !inclusive)) path.hi = IndexBound{c.value, inclusive};
            }
            consumed.insert(c.pred);
        }
    }

    if (!path.index) return path;
    for (size_t i : consumed) {
        if (!path.condition.empty()) path.condition += " AND ";
        path.condition += preds[i]->to_string();
    }
    for (auto it = consumed.rbegin(); it != consumed.rend(); ++it) preds.erase(preds.begin() + static_cast<long>(*it));
    return path;
}

struct Source {
    Table* table;
    std::string alias;
    size_t offset;
    size_t width;
    JoinType type;
    ExprPtr on;
};

size_t source_of_slot(const std::vector<Source>& sources, int slot) {
    for (size_t i = 0; i < sources.size(); ++i)
        if (static_cast<size_t>(slot) >= sources[i].offset &&
            static_cast<size_t>(slot) < sources[i].offset + sources[i].width)
            return i;
    throw DbError(ErrorCode::Internal, "slot outside of any source");
}

std::set<size_t> sources_used(const std::vector<Source>& sources, const Expr& e) {
    std::vector<int> slots;
    collect_slots(e, slots);
    std::set<size_t> out;
    for (int s : slots) out.insert(source_of_slot(sources, s));
    return out;
}

// Requested output order for a single-table query (ORDER BY one column).
struct OrderHint {
    int column;                    // column index in the table
    bool desc;
    std::optional<size_t> limit;   // LIMIT + OFFSET, if any
    bool satisfied = false;        // set by build_scan when the scan delivers this order
};

// Scan of one source with predicates (any binding) pushed down into it.
OpPtr build_scan(Source& src, std::vector<ExprPtr> preds, OrderHint* hint = nullptr) {
    Scope local;
    local.add_table(src.alias, src.table->schema());
    for (auto& p : preds) bind_expr(*p, local);
    AccessPath path = choose_access_path(*src.table, preds);
    if (hint) {
        // Read the index in order instead of sorting: either the range scan already
        // uses the ORDER BY column's index, or no index was chosen and one exists.
        bool same_index = path.index && path.index->column() == hint->column && path.eq_values.empty();
        Index* order_index = same_index ? path.index : (!path.index ? src.table->index_on(hint->column) : nullptr);
        if (order_index) {
            path.index = order_index;
            path.ordered = true;
            path.descending = hint->desc;
            path.include_nulls = !path.lo && !path.hi;
            if (preds.empty()) path.limit = hint->limit;  // nothing filters rows after the scan
            hint->satisfied = true;
        }
    }
    OpPtr op;
    if (path.uses_index()) op = std::make_unique<IndexScanOp>(*src.table, src.alias, std::move(path));
    else op = std::make_unique<SeqScanOp>(*src.table, src.alias);
    if (ExprPtr rest = join_conjuncts(std::move(preds))) op = std::make_unique<FilterOp>(std::move(op), std::move(rest));
    return op;
}

bool same_expr(const Expr& a, const Expr& b) {
    if ((a.kind == ExprKind::Column || a.kind == ExprKind::Slot) &&
        (b.kind == ExprKind::Column || b.kind == ExprKind::Slot))
        return a.slot >= 0 && a.slot == b.slot;
    return a.to_string() == b.to_string();
}

struct AggregateRewriter {
    const std::vector<ExprPtr>& groups;
    std::vector<AggregateSpec>& aggs;

    ExprPtr rewrite(const Expr& e) {
        for (size_t g = 0; g < groups.size(); ++g)
            if (same_expr(e, *groups[g])) return Expr::make_slot(static_cast<int>(g), e.to_string());
        if (e.kind == ExprKind::Function && is_aggregate_function(e.name)) {
            if (e.star && e.name != "COUNT") semantic("'*' is only valid in COUNT");
            for (const auto& a : e.args)
                if (contains_aggregate(*a)) semantic("aggregate function calls cannot be nested");
            if (!e.star && e.args.size() != 1) semantic(e.name + " takes exactly one argument");
            std::string display = e.to_string();
            for (size_t i = 0; i < aggs.size(); ++i)
                if (aggs[i].display == display) return Expr::make_slot(static_cast<int>(groups.size() + i), display);
            AggregateSpec spec;
            spec.function = e.name;
            spec.distinct = e.distinct;
            spec.display = display;
            if (!e.star) spec.arg = e.args[0]->clone();
            aggs.push_back(std::move(spec));
            return Expr::make_slot(static_cast<int>(groups.size() + aggs.size() - 1), display);
        }
        if (e.kind == ExprKind::Column)
            semantic("column '" + e.to_string() + "' must appear in GROUP BY or be used in an aggregate function");
        auto copy = std::make_unique<Expr>();
        copy->kind = e.kind;
        copy->literal = e.literal;
        copy->table = e.table;
        copy->name = e.name;
        copy->uop = e.uop;
        copy->bop = e.bop;
        copy->negated = e.negated;
        copy->distinct = e.distinct;
        copy->star = e.star;
        copy->slot = e.slot;
        for (const auto& a : e.args) copy->args.push_back(rewrite(*a));
        return copy;
    }
};

int64_t eval_count(const ExprPtr& e, const char* what) {
    if (!is_constant(*e)) semantic(std::string(what) + " must be a constant");
    Value v = eval_expr(*e, {});
    if (v.type() != DataType::Integer || v.as_int() < 0) semantic(std::string(what) + " must be a non-negative integer");
    return v.as_int();
}

}  // namespace

// ===========================================================================
// SELECT
// ===========================================================================

SelectPlan plan_select(Database& db, SelectStmt& s) {
    // ---- 1. sources and the combined scope ----
    std::vector<Source> sources;
    Scope full;
    std::set<std::string> aliases;
    auto add_source = [&](const TableRef& ref, JoinType type, ExprPtr on) {
        Table& t = db.table(ref.name);
        const std::string& alias = ref.effective_name();
        if (!aliases.insert(alias).second) semantic("table name or alias '" + alias + "' specified more than once");
        sources.push_back({&t, alias, full.size(), t.schema().columns.size(), type, std::move(on)});
        full.add_table(alias, t.schema());
    };
    if (s.from) {
        add_source(*s.from, JoinType::Inner, nullptr);
        for (auto& j : s.joins) add_source(j.table, j.type, std::move(j.on));
    }

    // ---- 2. expand '*' and bind select items ----
    std::vector<SelectItem> items;
    for (auto& item : s.items) {
        if (!item.star) {
            items.push_back(std::move(item));
            continue;
        }
        if (sources.empty()) semantic("SELECT * requires a FROM clause");
        if (!item.star_table.empty() && !full.has_table(item.star_table))
            semantic("unknown table or alias '" + item.star_table + "'");
        for (const auto& c : full.columns()) {
            if (!item.star_table.empty() && c.table != item.star_table) continue;
            SelectItem expanded;
            expanded.expr = Expr::make_column(c.table, c.name);
            expanded.alias = c.name;
            items.push_back(std::move(expanded));
        }
    }
    for (auto& item : items) bind_expr(*item.expr, full);

    // ---- 3. WHERE: split into conjuncts and assign each to a join step ----
    std::vector<std::vector<ExprPtr>> step_preds(std::max<size_t>(sources.size(), 1));
    if (s.where) {
        if (contains_aggregate(*s.where)) semantic("aggregate functions are not allowed in WHERE");
        std::vector<ExprPtr> conj;
        split_conjuncts(std::move(s.where), conj);
        for (auto& c : conj) {
            bind_expr(*c, full);
            auto used = sources_used(sources, *c);
            size_t step = used.empty() ? 0 : *used.rbegin();
            step_preds[step].push_back(std::move(c));
        }
    }

    // ---- 4a. can a single-table ORDER BY come straight from an index? ----
    std::optional<OrderHint> order_hint;
    bool may_aggregate = !s.group_by.empty() || s.having != nullptr;
    for (const auto& item : items) may_aggregate = may_aggregate || contains_aggregate(*item.expr);
    if (sources.size() == 1 && s.order_by.size() == 1 && !may_aggregate && !s.distinct) {
        const Expr& e = *s.order_by[0].expr;
        const Expr* col = nullptr;
        ExprPtr bound;
        if (e.kind == ExprKind::Literal && e.literal.type() == DataType::Integer) {
            int64_t k = e.literal.as_int();
            if (k >= 1 && k <= static_cast<int64_t>(items.size())) col = items[static_cast<size_t>(k - 1)].expr.get();
        } else {
            if (e.kind == ExprKind::Column && e.table.empty())
                for (const auto& item : items)
                    if (item.alias == e.name) col = item.expr.get();
            if (!col) {
                bound = e.clone();
                try {
                    bind_expr(*bound, full);
                    col = bound.get();
                } catch (const DbError&) {
                    // Not a plain column (e.g. an alias of an expression): handled by the normal path.
                }
            }
        }
        if (col && col->kind == ExprKind::Column && col->slot >= 0) {
            std::optional<size_t> limit;
            if (s.limit) limit = static_cast<size_t>(eval_count(s.limit, "LIMIT") + (s.offset ? eval_count(s.offset, "OFFSET") : 0));
            order_hint = OrderHint{col->slot, s.order_by[0].desc, limit};
        }
    }

    // ---- 4. scans and joins (left-deep, in FROM order) ----
    OpPtr op;
    if (sources.empty()) {
        op = std::make_unique<SingleRowOp>();
        if (ExprPtr f = join_conjuncts(std::move(step_preds[0]))) op = std::make_unique<FilterOp>(std::move(op), std::move(f));
    } else {
        op = build_scan(sources[0], std::move(step_preds[0]), order_hint ? &*order_hint : nullptr);
        Scope prefix;
        prefix.add_table(sources[0].alias, sources[0].table->schema());
        for (size_t k = 1; k < sources.size(); ++k) {
            Source& src = sources[k];
            prefix.add_table(src.alias, src.table->schema());
            bool left_join = src.type == JoinType::Left;

            std::vector<ExprPtr> preds, post;
            if (src.on) {
                if (contains_aggregate(*src.on)) semantic("aggregate functions are not allowed in ON");
                split_conjuncts(std::move(src.on), preds);
                for (auto& p : preds) bind_expr(*p, prefix);
            }
            for (auto& p : step_preds[k]) (left_join ? post : preds).push_back(std::move(p));

            std::vector<ExprPtr> right_only, join_preds;
            for (auto& p : preds) {
                auto used = sources_used(sources, *p);
                if (used.size() == 1 && *used.begin() == k) right_only.push_back(std::move(p));
                else join_preds.push_back(std::move(p));
            }

            // Look for "left_expr = right.indexed_column" to drive an index join.
            Index* join_index = nullptr;
            ExprPtr key;
            for (size_t i = 0; i < join_preds.size() && !join_index; ++i) {
                Expr& p = *join_preds[i];
                if (p.kind != ExprKind::Binary || p.bop != BinaryOp::Eq) continue;
                for (int side = 0; side < 2 && !join_index; ++side) {
                    Expr& col = *p.args[static_cast<size_t>(side)];
                    Expr& other = *p.args[static_cast<size_t>(1 - side)];
                    if (col.kind != ExprKind::Column || source_of_slot(sources, col.slot) != k) continue;
                    auto other_used = sources_used(sources, other);
                    if (other_used.empty() || *other_used.rbegin() >= k) continue;
                    Index* idx = src.table->index_on(col.slot - static_cast<int>(src.offset));
                    if (!idx) continue;
                    join_index = idx;
                    key = std::move(p.args[static_cast<size_t>(1 - side)]);
                    join_preds.erase(join_preds.begin() + static_cast<long>(i));
                }
            }

            if (join_index) {
                for (auto& p : right_only) join_preds.push_back(std::move(p));
                op = std::make_unique<IndexJoinOp>(std::move(op), *src.table, src.alias, join_index, std::move(key),
                                                   join_conjuncts(std::move(join_preds)), left_join);
            } else {
                OpPtr right = build_scan(src, std::move(right_only));
                op = std::make_unique<NestedLoopJoinOp>(std::move(op), std::move(right),
                                                        join_conjuncts(std::move(join_preds)), left_join, src.width);
            }
            if (ExprPtr f = join_conjuncts(std::move(post))) op = std::make_unique<FilterOp>(std::move(op), std::move(f));
        }
    }

    // ---- 5. resolve ORDER BY items against the output columns ----
    auto output_name = [](const SelectItem& item) {
        if (!item.alias.empty()) return item.alias;
        if (item.expr->kind == ExprKind::Column) return item.expr->name;
        return item.expr->to_string();
    };
    // Names are taken now: aggregation later rewrites the expressions.
    std::vector<std::string> output_names;
    for (const auto& item : items) output_names.push_back(output_name(item));
    struct OrderRef {
        int output = -1;  // index into select items, or -1 for a hidden expression
        ExprPtr hidden;
        bool desc;
        std::string display;
    };
    std::vector<OrderRef> order;
    for (auto& o : s.order_by) {
        OrderRef ref;
        ref.desc = o.desc;
        ref.display = o.expr->to_string();
        const Expr& e = *o.expr;
        if (e.kind == ExprKind::Literal && e.literal.type() == DataType::Integer) {
            int64_t k = e.literal.as_int();
            if (k < 1 || k > static_cast<int64_t>(items.size())) semantic("ORDER BY position " + std::to_string(k) + " is out of range");
            ref.output = static_cast<int>(k - 1);
        } else {
            if (e.kind == ExprKind::Column && e.table.empty()) {
                for (size_t i = 0; i < items.size(); ++i)
                    if (output_name(items[i]) == e.name && !items[i].alias.empty()) ref.output = static_cast<int>(i);
            }
            if (ref.output < 0) {
                bind_expr(*o.expr, full);
                for (size_t i = 0; i < items.size() && ref.output < 0; ++i)
                    if (same_expr(*o.expr, *items[i].expr)) ref.output = static_cast<int>(i);
                if (ref.output < 0) ref.hidden = std::move(o.expr);
            }
        }
        order.push_back(std::move(ref));
    }

    // ---- 6. aggregation ----
    bool has_agg = !s.group_by.empty() || s.having != nullptr;
    for (const auto& item : items) has_agg = has_agg || contains_aggregate(*item.expr);
    for (const auto& o : order) has_agg = has_agg || (o.hidden && contains_aggregate(*o.hidden));

    Scope out_scope = full;  // scope used for type inference of the projection
    if (has_agg) {
        std::vector<ExprPtr> groups;
        for (auto& g : s.group_by) {
            if (contains_aggregate(*g)) semantic("aggregate functions are not allowed in GROUP BY");
            if (g->kind == ExprKind::Literal && g->literal.type() == DataType::Integer) {
                int64_t k = g->literal.as_int();
                if (k < 1 || k > static_cast<int64_t>(items.size())) semantic("GROUP BY position out of range");
                if (contains_aggregate(*items[static_cast<size_t>(k - 1)].expr)) semantic("cannot GROUP BY an aggregate");
                g = items[static_cast<size_t>(k - 1)].expr->clone();
            }
            bind_expr(*g, full);
            groups.push_back(std::move(g));
        }
        std::vector<AggregateSpec> aggs;
        AggregateRewriter rw{groups, aggs};
        for (auto& item : items) item.expr = rw.rewrite(*item.expr);
        for (auto& o : order)
            if (o.hidden) o.hidden = rw.rewrite(*o.hidden);
        ExprPtr having;
        if (s.having) {
            bind_expr(*s.having, full);
            having = rw.rewrite(*s.having);
        }

        Scope agg_scope;
        for (const auto& g : groups) agg_scope.add({"", g->to_string(), infer_type(*g, full)});
        for (const auto& a : aggs) {
            DataType t = DataType::Integer;
            if (a.function == "AVG") t = DataType::Real;
            else if (a.function != "COUNT") t = infer_type(*a.arg, full);
            agg_scope.add({"", a.display, t});
        }
        out_scope = agg_scope;

        op = std::make_unique<AggregateOp>(std::move(op), std::move(groups), std::move(aggs));
        if (having) op = std::make_unique<FilterOp>(std::move(op), std::move(having));
    }

    // ---- 7. projection (+ hidden sort keys), distinct, sort, limit ----
    SelectPlan plan;
    std::vector<ExprPtr> proj;
    for (auto& item : items) {
        plan.column_names.push_back(output_names[proj.size()]);
        plan.column_types.push_back(infer_type(*item.expr, out_scope));
        proj.push_back(std::move(item.expr));
    }
    size_t visible = proj.size();
    std::vector<SortKey> keys;
    for (auto& o : order) {
        if (o.hidden) {
            keys.push_back({proj.size(), o.desc, o.display});
            proj.push_back(std::move(o.hidden));
        } else {
            keys.push_back({static_cast<size_t>(o.output), o.desc, o.display});
        }
    }
    if (s.distinct && proj.size() > visible)
        semantic("for SELECT DISTINCT, ORDER BY expressions must appear in the select list");

    op = std::make_unique<ProjectOp>(std::move(op), std::move(proj));
    if (s.distinct) op = std::make_unique<DistinctOp>(std::move(op));
    std::optional<int64_t> limit;
    int64_t offset = 0;
    if (s.limit) limit = eval_count(s.limit, "LIMIT");
    if (s.offset) offset = eval_count(s.offset, "OFFSET");
    bool sorted_by_scan = order_hint && order_hint->satisfied;
    if (!keys.empty() && !sorted_by_scan) {
        std::optional<size_t> top_n;
        if (limit) top_n = static_cast<size_t>(*limit + offset);
        op = std::make_unique<SortOp>(std::move(op), std::move(keys), top_n);
    }
    if (s.limit || s.offset) op = std::make_unique<LimitOp>(std::move(op), limit, offset);
    // Hidden sort keys were appended after the visible columns; strip them.
    {
        size_t total_hidden = 0;
        for (const auto& o : order) total_hidden += o.output < 0 ? 1 : 0;
        if (total_hidden) op = std::make_unique<TruncateOp>(std::move(op), visible);
    }
    plan.root = std::move(op);
    return plan;
}

// ===========================================================================
// UPDATE / DELETE scans
// ===========================================================================

TableScanPlan plan_table_scan(Table& table, ExprPtr where) {
    TableScanPlan plan;
    plan.table = &table;
    std::vector<ExprPtr> preds;
    if (where) {
        if (contains_aggregate(*where)) semantic("aggregate functions are not allowed in WHERE");
        split_conjuncts(std::move(where), preds);
        Scope scope;
        scope.add_table(table.name(), table.schema());
        for (auto& p : preds) bind_expr(*p, scope);
    }
    plan.path = choose_access_path(table, preds);
    plan.filter = join_conjuncts(std::move(preds));
    return plan;
}

std::string TableScanPlan::explain(const std::string& action) const {
    std::string out = action + " " + table->name() + "\n";
    int depth = 1;
    if (filter) {
        out += "  -> Filter " + filter->to_string() + "\n";
        depth = 2;
    }
    out += std::string(static_cast<size_t>(depth) * 2, ' ') + "-> ";
    if (path.uses_index())
        out += "IndexScan " + table->name() + " USING " + path.index->def().name + " " + path.condition + "\n";
    else
        out += "SeqScan " + table->name() + "\n";
    return out;
}

std::vector<std::pair<RID, Row>> TableScanPlan::collect() const {
    std::vector<std::pair<RID, Row>> out;
    auto consider = [&](RID rid, Row row) {
        if (!filter || eval_predicate(*filter, row)) out.emplace_back(rid, std::move(row));
    };
    if (path.uses_index()) {
        for (RID rid : path.rids())
            if (auto row = table->read(rid)) consider(rid, std::move(*row));
    } else {
        RID rid{0, 0};
        std::string rec;
        while (table->heap().next(rid, rid, rec)) {
            consider(rid, decode_row(rec));
            if (++rid.slot == 0) ++rid.page;
        }
    }
    return out;
}

}  // namespace atlas
