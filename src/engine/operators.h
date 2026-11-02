#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "engine/database.h"
#include "engine/expr.h"

namespace atlas {

// How a single table is read: full scan, or through an index.
struct AccessPath {
    Index* index = nullptr;
    std::vector<Value> eq_values;  // point lookups (col = v, col IN (...))
    std::optional<IndexBound> lo, hi;
    std::string condition;         // human readable, for EXPLAIN

    // Ordered scans deliver rows in index order, so the plan needs no Sort.
    bool ordered = false;
    bool descending = false;
    bool include_nulls = false;      // unbounded ordered scans return NULL keys too
    std::optional<size_t> limit;     // stop early (only when nothing filters the rows)

    bool uses_index() const { return index != nullptr; }
    std::vector<RID> rids() const;
};

// Pull-based (Volcano) iterator.
class Operator {
public:
    virtual ~Operator() = default;
    virtual void open() = 0;
    virtual bool next(Row& out) = 0;
    virtual std::string describe() const = 0;
    virtual std::vector<const Operator*> children() const { return {}; }

    std::string explain(int depth = 0) const;
};

using OpPtr = std::unique_ptr<Operator>;

class SeqScanOp : public Operator {
public:
    SeqScanOp(Table& t, std::string alias) : table_(t), alias_(std::move(alias)) {}
    void open() override { cursor_ = RID{0, 0}; done_ = false; }
    bool next(Row& out) override;
    std::string describe() const override;

private:
    Table& table_;
    std::string alias_;
    RID cursor_;
    bool done_ = false;
    std::string buf_;
};

class IndexScanOp : public Operator {
public:
    IndexScanOp(Table& t, std::string alias, AccessPath path)
        : table_(t), alias_(std::move(alias)), path_(std::move(path)) {}
    void open() override;
    bool next(Row& out) override;
    std::string describe() const override;

private:
    Table& table_;
    std::string alias_;
    AccessPath path_;
    std::vector<RID> rids_;
    size_t pos_ = 0;
};

// Produces exactly one empty row (SELECT without FROM).
class SingleRowOp : public Operator {
public:
    void open() override { done_ = false; }
    bool next(Row& out) override;
    std::string describe() const override { return "Result"; }

private:
    bool done_ = false;
};

class FilterOp : public Operator {
public:
    FilterOp(OpPtr child, ExprPtr pred) : child_(std::move(child)), pred_(std::move(pred)) {}
    void open() override { child_->open(); }
    bool next(Row& out) override;
    std::string describe() const override { return "Filter " + pred_->to_string(); }
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OpPtr child_;
    ExprPtr pred_;
};

class NestedLoopJoinOp : public Operator {
public:
    NestedLoopJoinOp(OpPtr left, OpPtr right, ExprPtr cond, bool left_join, size_t right_width)
        : left_(std::move(left)), right_(std::move(right)), cond_(std::move(cond)), left_join_(left_join),
          right_width_(right_width) {}
    void open() override;
    bool next(Row& out) override;
    std::string describe() const override;
    std::vector<const Operator*> children() const override { return {left_.get(), right_.get()}; }

private:
    OpPtr left_, right_;
    ExprPtr cond_;
    bool left_join_;
    size_t right_width_;
    std::vector<Row> right_rows_;
    Row left_row_;
    bool have_left_ = false;
    bool matched_ = false;
    size_t right_pos_ = 0;
};

// For each left row, looks up matching right rows through an index.
class IndexJoinOp : public Operator {
public:
    IndexJoinOp(OpPtr left, Table& right, std::string alias, Index* index, ExprPtr key, ExprPtr residual,
                bool left_join)
        : left_(std::move(left)), right_(right), alias_(std::move(alias)), index_(index), key_(std::move(key)),
          residual_(std::move(residual)), left_join_(left_join) {}
    void open() override;
    bool next(Row& out) override;
    std::string describe() const override;
    std::vector<const Operator*> children() const override { return {left_.get()}; }

private:
    OpPtr left_;
    Table& right_;
    std::string alias_;
    Index* index_;
    ExprPtr key_;
    ExprPtr residual_;
    bool left_join_;
    Row left_row_;
    bool have_left_ = false;
    bool matched_ = false;
    std::vector<RID> matches_;
    size_t pos_ = 0;
};

struct AggregateSpec {
    std::string function;  // COUNT, SUM, AVG, MIN, MAX
    ExprPtr arg;           // null for COUNT(*)
    bool distinct = false;
    std::string display;
};

class AggregateOp : public Operator {
public:
    AggregateOp(OpPtr child, std::vector<ExprPtr> groups, std::vector<AggregateSpec> aggs)
        : child_(std::move(child)), groups_(std::move(groups)), aggs_(std::move(aggs)) {}
    void open() override;
    bool next(Row& out) override;
    std::string describe() const override;
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OpPtr child_;
    std::vector<ExprPtr> groups_;
    std::vector<AggregateSpec> aggs_;
    std::vector<Row> results_;
    size_t pos_ = 0;
};

class ProjectOp : public Operator {
public:
    ProjectOp(OpPtr child, std::vector<ExprPtr> exprs) : child_(std::move(child)), exprs_(std::move(exprs)) {}
    void open() override { child_->open(); }
    bool next(Row& out) override;
    std::string describe() const override;
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OpPtr child_;
    std::vector<ExprPtr> exprs_;
    Row in_;
};

class DistinctOp : public Operator {
public:
    explicit DistinctOp(OpPtr child) : child_(std::move(child)) {}
    void open() override {
        seen_.clear();
        child_->open();
    }
    bool next(Row& out) override;
    std::string describe() const override { return "Distinct"; }
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OpPtr child_;
    std::unordered_set<Row, RowHash> seen_;
};

struct SortKey {
    size_t column;
    bool desc;
    std::string display;
};

class SortOp : public Operator {
public:
    // With top_n, only the first N rows of the sorted output are kept (heap of size N).
    SortOp(OpPtr child, std::vector<SortKey> keys, std::optional<size_t> top_n = std::nullopt)
        : child_(std::move(child)), keys_(std::move(keys)), top_n_(top_n) {}
    void open() override;
    bool next(Row& out) override;
    std::string describe() const override;
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OpPtr child_;
    std::vector<SortKey> keys_;
    std::optional<size_t> top_n_;
    std::vector<Row> rows_;
    size_t pos_ = 0;
};

class LimitOp : public Operator {
public:
    LimitOp(OpPtr child, std::optional<int64_t> limit, int64_t offset)
        : child_(std::move(child)), limit_(limit), offset_(offset) {}
    void open() override {
        child_->open();
        emitted_ = 0;
        skipped_ = 0;
    }
    bool next(Row& out) override;
    std::string describe() const override;
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OpPtr child_;
    std::optional<int64_t> limit_;
    int64_t offset_;
    int64_t emitted_ = 0;
    int64_t skipped_ = 0;
};

// Drops trailing helper columns (e.g. ORDER BY keys not in the select list).
class TruncateOp : public Operator {
public:
    TruncateOp(OpPtr child, size_t width) : child_(std::move(child)), width_(width) {}
    void open() override { child_->open(); }
    bool next(Row& out) override {
        if (!child_->next(out)) return false;
        out.resize(width_);
        return true;
    }
    std::string describe() const override { return "Project (drop sort keys)"; }
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OpPtr child_;
    size_t width_;
};

}  // namespace atlas
