#pragma once

#include <string>
#include <utility>
#include <vector>

#include "engine/database.h"
#include "engine/operators.h"
#include "sql/ast.h"

namespace atlas {

struct SelectPlan {
    OpPtr root;
    std::vector<std::string> column_names;
    std::vector<DataType> column_types;
};

// Builds an executable operator tree for a SELECT. Consumes parts of `stmt`.
SelectPlan plan_select(Database& db, SelectStmt& stmt);

// Plan for reading the rows of one table that match a WHERE clause (UPDATE / DELETE).
struct TableScanPlan {
    Table* table = nullptr;
    AccessPath path;
    ExprPtr filter;  // bound to the table's own row layout

    std::string explain(const std::string& action) const;
    // Collects matching rows up front, so the caller may modify the table safely.
    std::vector<std::pair<RID, Row>> collect() const;
};

TableScanPlan plan_table_scan(Table& table, ExprPtr where);

}  // namespace atlas
