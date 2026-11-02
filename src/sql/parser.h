#pragma once

#include <string_view>
#include <vector>

#include "sql/ast.h"

namespace atlas {

// Parses a script of one or more ';'-separated statements.
std::vector<Statement> parse_sql(std::string_view sql);

// Parses exactly one statement (a trailing ';' is allowed).
Statement parse_statement(std::string_view sql);

// Parses a standalone expression (used for stored DEFAULT/CHECK clauses).
ExprPtr parse_expression(std::string_view sql);

}  // namespace atlas
