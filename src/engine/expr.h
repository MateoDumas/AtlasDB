#pragma once

#include <string>
#include <vector>

#include "catalog/schema.h"
#include "common/value.h"
#include "sql/ast.h"

namespace atlas {

struct ScopeColumn {
    std::string table;  // alias (or table name) used to qualify the column
    std::string name;
    DataType type = DataType::Null;
};

// Describes the layout of the rows an expression is evaluated against.
class Scope {
public:
    void add_table(const std::string& alias, const TableSchema& schema);
    void add(ScopeColumn c) { cols_.push_back(std::move(c)); }
    // Returns the slot of [table.]name; throws DbError(Semantic) if unknown or ambiguous.
    int resolve(const std::string& table, const std::string& name) const;
    bool has_table(const std::string& alias) const;
    const std::vector<ScopeColumn>& columns() const { return cols_; }
    size_t size() const { return cols_.size(); }

private:
    std::vector<ScopeColumn> cols_;
};

// Resolves column references to row slots. Aggregate arguments are bound too.
void bind_expr(Expr& e, const Scope& scope);

Value eval_expr(const Expr& e, const Row& row);
// WHERE/ON/HAVING semantics: NULL counts as false. Non-boolean results are an error.
bool eval_predicate(const Expr& e, const Row& row);

bool contains_aggregate(const Expr& e);
bool contains_columns(const Expr& e);  // any Column/Slot reference
void collect_slots(const Expr& e, std::vector<int>& out);
DataType infer_type(const Expr& e, const Scope& scope);

// Splits a conjunction into its AND-ed terms (moving them out of `e`).
void split_conjuncts(ExprPtr e, std::vector<ExprPtr>& out);
// Re-joins terms with AND; returns nullptr for an empty list.
ExprPtr join_conjuncts(std::vector<ExprPtr> terms);

bool like_match(std::string_view text, std::string_view pattern);

}  // namespace atlas
