#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/value.h"

namespace atlas {

// Outcome of executing one statement. Statements that produce rows set
// has_rows; others only carry a command tag such as "INSERT 3".
struct QueryResult {
    bool has_rows = false;
    std::vector<std::string> columns;
    std::vector<DataType> types;
    std::vector<Row> rows;
    std::string tag;
    uint64_t affected = 0;
};

// Renders a result as an ASCII table (or just the tag for commands).
std::string format_result(const QueryResult& r);

}  // namespace atlas
