#include "engine/operators.h"

#include <algorithm>
#include <map>
#include <set>

#include "common/error.h"
#include "storage/row_codec.h"

namespace atlas {

std::vector<RID> AccessPath::rids() const {
    if (!index) return {};
    if (!eq_values.empty()) {
        std::vector<RID> out;
        std::set<Value, decltype([](const Value& a, const Value& b) { return compare_values(a, b) < 0; })> seen;
        for (const auto& v : eq_values) {
            if (!seen.insert(v).second) continue;
            auto r = index->lookup(v);
            out.insert(out.end(), r.begin(), r.end());
        }
        return out;
    }
    if (ordered) return index->scan_ordered(lo, hi, descending, limit.value_or(SIZE_MAX), include_nulls);
    return index->range(lo, hi);
}

std::string Operator::explain(int depth) const {
    std::string out(static_cast<size_t>(depth) * 2, ' ');
    out += (depth ? "-> " : "") + describe() + "\n";
    for (const Operator* c : children()) out += c->explain(depth + 1);
    return out;
}

// ---- scans ----------------------------------------------------------------

bool SeqScanOp::next(Row& out) {
    if (done_) return false;
    RID rid;
    if (!table_.next_row(cursor_, rid, out)) {
        done_ = true;
        return false;
    }
    cursor_ = rid;
    if (++cursor_.slot == 0) ++cursor_.page;
    return true;
}

std::string SeqScanOp::describe() const {
    std::string s = "SeqScan " + table_.name();
    if (alias_ != table_.name()) s += " AS " + alias_;
    return s;
}

void IndexScanOp::open() {
    rids_ = path_.rids();
    pos_ = 0;
}

bool IndexScanOp::next(Row& out) {
    while (pos_ < rids_.size()) {
        if (auto row = table_.read(rids_[pos_++])) {
            out = std::move(*row);
            return true;
        }
    }
    return false;
}

std::string IndexScanOp::describe() const {
    std::string s = "IndexScan " + table_.name();
    if (alias_ != table_.name()) s += " AS " + alias_;
    s += " USING " + path_.index->def().name;
    if (!path_.condition.empty()) s += " " + path_.condition;
    if (path_.ordered) s += std::string(" ORDER BY ") + path_.index->def().column + (path_.descending ? " DESC" : "");
    if (path_.limit) s += " LIMIT " + std::to_string(*path_.limit);
    return s;
}

bool SingleRowOp::next(Row& out) {
    if (done_) return false;
    done_ = true;
    out.clear();
    return true;
}

bool FilterOp::next(Row& out) {
    while (child_->next(out))
        if (eval_predicate(*pred_, out)) return true;
    return false;
}

// ---- joins ----------------------------------------------------------------

void NestedLoopJoinOp::open() {
    left_->open();
    right_->open();
    right_rows_.clear();
    Row r;
    while (right_->next(r)) right_rows_.push_back(r);
    have_left_ = false;
}

bool NestedLoopJoinOp::next(Row& out) {
    while (true) {
        if (!have_left_) {
            if (!left_->next(left_row_)) return false;
            have_left_ = true;
            matched_ = false;
            right_pos_ = 0;
        }
        while (right_pos_ < right_rows_.size()) {
            const Row& r = right_rows_[right_pos_++];
            out = left_row_;
            out.insert(out.end(), r.begin(), r.end());
            if (!cond_ || eval_predicate(*cond_, out)) {
                matched_ = true;
                return true;
            }
        }
        have_left_ = false;
        if (left_join_ && !matched_) {
            out = left_row_;
            out.resize(left_row_.size() + right_width_);
            return true;
        }
    }
}

std::string NestedLoopJoinOp::describe() const {
    std::string s = left_join_ ? "NestedLoopLeftJoin" : "NestedLoopJoin";
    if (cond_) s += " ON " + cond_->to_string();
    return s;
}

void IndexJoinOp::open() {
    left_->open();
    have_left_ = false;
}

bool IndexJoinOp::next(Row& out) {
    size_t right_width = right_.schema().columns.size();
    while (true) {
        if (!have_left_) {
            if (!left_->next(left_row_)) return false;
            have_left_ = true;
            matched_ = false;
            matches_ = index_->lookup(eval_expr(*key_, left_row_));
            pos_ = 0;
        }
        while (pos_ < matches_.size()) {
            auto r = right_.read(matches_[pos_++]);
            if (!r) continue;
            out = left_row_;
            out.insert(out.end(), r->begin(), r->end());
            if (!residual_ || eval_predicate(*residual_, out)) {
                matched_ = true;
                return true;
            }
        }
        have_left_ = false;
        if (left_join_ && !matched_) {
            out = left_row_;
            out.resize(left_row_.size() + right_width);
            return true;
        }
    }
}

std::string IndexJoinOp::describe() const {
    std::string s = left_join_ ? "IndexLeftJoin " : "IndexJoin ";
    s += right_.name();
    if (alias_ != right_.name()) s += " AS " + alias_;
    s += " USING " + index_->def().name + " (" + index_->def().column + " = " + key_->to_string() + ")";
    if (residual_) s += " FILTER " + residual_->to_string();
    return s;
}

// ---- aggregation ----------------------------------------------------------

namespace {

struct AggState {
    int64_t count = 0;
    bool real_sum = false;
    int64_t int_sum = 0;
    double real_total = 0;
    Value best;  // MIN / MAX
    std::unordered_set<Value, ValueHash> distinct_seen;
};

void accumulate(const AggregateSpec& spec, AggState& st, const Row& row) {
    if (!spec.arg) {  // COUNT(*)
        ++st.count;
        return;
    }
    Value v = eval_expr(*spec.arg, row);
    if (v.is_null()) return;
    if (spec.distinct && !st.distinct_seen.insert(v).second) return;
    ++st.count;
    const std::string& f = spec.function;
    if (f == "SUM" || f == "AVG") {
        if (!v.is_numeric()) throw DbError(ErrorCode::Type, f + " requires numeric values");
        if (v.type() == DataType::Real && !st.real_sum) {
            st.real_sum = true;
            st.real_total = static_cast<double>(st.int_sum);
        }
        if (st.real_sum) {
            st.real_total += v.as_real();
        } else {
#if defined(__GNUC__) || defined(__clang__)
            if (__builtin_add_overflow(st.int_sum, v.as_int(), &st.int_sum))
                throw DbError(ErrorCode::Type, "integer overflow in " + f);
#else
            st.int_sum += v.as_int();
#endif
        }
    } else if (f == "MIN" || f == "MAX") {
        if (st.best.is_null()) {
            st.best = v;
        } else {
            int c = compare_values(v, st.best);
            if ((f == "MIN" && c < 0) || (f == "MAX" && c > 0)) st.best = v;
        }
    }
}

Value finish(const AggregateSpec& spec, const AggState& st) {
    const std::string& f = spec.function;
    if (f == "COUNT") return Value::integer(st.count);
    if (f == "MIN" || f == "MAX") return st.best;
    if (st.count == 0) return Value::null();
    if (f == "SUM") return st.real_sum ? Value::real(st.real_total) : Value::integer(st.int_sum);
    double total = st.real_sum ? st.real_total : static_cast<double>(st.int_sum);
    return Value::real(total / static_cast<double>(st.count));
}

}  // namespace

void AggregateOp::open() {
    child_->open();
    std::map<Row, std::vector<AggState>, RowLess> groups;
    Row in;
    while (child_->next(in)) {
        Row key;
        key.reserve(groups_.size());
        for (const auto& g : groups_) key.push_back(eval_expr(*g, in));
        auto it = groups.find(key);
        if (it == groups.end()) it = groups.emplace(std::move(key), std::vector<AggState>(aggs_.size())).first;
        for (size_t i = 0; i < aggs_.size(); ++i) accumulate(aggs_[i], it->second[i], in);
    }
    // An aggregate without GROUP BY always yields one row, even for empty input.
    if (groups.empty() && groups_.empty()) groups.emplace(Row{}, std::vector<AggState>(aggs_.size()));

    results_.clear();
    for (const auto& [key, states] : groups) {
        Row out = key;
        for (size_t i = 0; i < aggs_.size(); ++i) out.push_back(finish(aggs_[i], states[i]));
        results_.push_back(std::move(out));
    }
    pos_ = 0;
}

bool AggregateOp::next(Row& out) {
    if (pos_ >= results_.size()) return false;
    out = results_[pos_++];
    return true;
}

std::string AggregateOp::describe() const {
    std::string s = "Aggregate";
    if (!groups_.empty()) {
        s += " GROUP BY ";
        for (size_t i = 0; i < groups_.size(); ++i) s += (i ? ", " : "") + groups_[i]->to_string();
    }
    if (!aggs_.empty()) {
        s += " [";
        for (size_t i = 0; i < aggs_.size(); ++i) s += (i ? ", " : "") + aggs_[i].display;
        s += "]";
    }
    return s;
}

// ---- projection, distinct, sort, limit ---------------------------------------

bool ProjectOp::next(Row& out) {
    if (!child_->next(in_)) return false;
    out.clear();
    out.reserve(exprs_.size());
    for (const auto& e : exprs_) out.push_back(eval_expr(*e, in_));
    return true;
}

std::string ProjectOp::describe() const {
    std::string s = "Project [";
    for (size_t i = 0; i < exprs_.size(); ++i) s += (i ? ", " : "") + exprs_[i]->to_string();
    return s + "]";
}

bool DistinctOp::next(Row& out) {
    while (child_->next(out))
        if (seen_.insert(out).second) return true;
    return false;
}

void SortOp::open() {
    child_->open();
    rows_.clear();
    pos_ = 0;
    auto key_less = [&](const Row& a, const Row& b) {
        for (const auto& k : keys_) {
            int c = compare_values(a[k.column], b[k.column]);
            if (c != 0) return k.desc ? c > 0 : c < 0;
        }
        return false;
    };
    Row r;
    if (!top_n_) {
        while (child_->next(r)) rows_.push_back(r);
        std::stable_sort(rows_.begin(), rows_.end(), key_less);
        return;
    }
    // Top-N: keep the N smallest rows in a max-heap. The arrival number breaks
    // ties so the result matches a stable sort.
    size_t n = *top_n_;
    using Entry = std::pair<Row, size_t>;
    auto entry_less = [&](const Entry& a, const Entry& b) {
        if (key_less(a.first, b.first)) return true;
        if (key_less(b.first, a.first)) return false;
        return a.second < b.second;
    };
    std::vector<Entry> heap;
    heap.reserve(n);
    for (size_t seq = 0; child_->next(r); ++seq) {
        if (n == 0) continue;
        if (heap.size() < n) {
            heap.emplace_back(r, seq);
            std::push_heap(heap.begin(), heap.end(), entry_less);
        } else if (entry_less({r, seq}, heap.front())) {
            std::pop_heap(heap.begin(), heap.end(), entry_less);
            heap.back() = {r, seq};
            std::push_heap(heap.begin(), heap.end(), entry_less);
        }
    }
    std::sort_heap(heap.begin(), heap.end(), entry_less);
    for (auto& e : heap) rows_.push_back(std::move(e.first));
}

bool SortOp::next(Row& out) {
    if (pos_ >= rows_.size()) return false;
    out = std::move(rows_[pos_++]);
    return true;
}

std::string SortOp::describe() const {
    std::string s = top_n_ ? "TopN " + std::to_string(*top_n_) + " [" : "Sort [";
    for (size_t i = 0; i < keys_.size(); ++i) s += (i ? ", " : "") + keys_[i].display + (keys_[i].desc ? " DESC" : "");
    return s + "]";
}

bool LimitOp::next(Row& out) {
    if (limit_ && emitted_ >= *limit_) return false;
    while (skipped_ < offset_) {
        if (!child_->next(out)) return false;
        ++skipped_;
    }
    if (!child_->next(out)) return false;
    ++emitted_;
    return true;
}

std::string LimitOp::describe() const {
    std::string s = "Limit";
    if (limit_) s += " " + std::to_string(*limit_);
    if (offset_) s += " OFFSET " + std::to_string(offset_);
    return s;
}

}  // namespace atlas
