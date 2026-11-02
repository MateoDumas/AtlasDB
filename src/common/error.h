#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace atlas {

enum class ErrorCode {
    Syntax,       // the SQL text could not be parsed
    Semantic,     // unknown table/column, ambiguous reference, misuse of SQL
    Constraint,   // integrity constraint violated
    Type,         // incompatible types / failed cast
    Transaction,  // illegal transaction state transition
    Locked,       // could not obtain the database lock in time
    IO,           // operating system I/O failure
    Corruption,   // on-disk data failed validation
    Protocol,     // malformed wire-protocol message
    Internal,     // bug or unexpected state
};

constexpr std::string_view error_code_name(ErrorCode c) {
    switch (c) {
        case ErrorCode::Syntax: return "syntax_error";
        case ErrorCode::Semantic: return "semantic_error";
        case ErrorCode::Constraint: return "constraint_violation";
        case ErrorCode::Type: return "type_error";
        case ErrorCode::Transaction: return "transaction_error";
        case ErrorCode::Locked: return "locked";
        case ErrorCode::IO: return "io_error";
        case ErrorCode::Corruption: return "corruption";
        case ErrorCode::Protocol: return "protocol_error";
        case ErrorCode::Internal: return "internal_error";
    }
    return "internal_error";
}

inline ErrorCode error_code_from_name(std::string_view name) {
    for (int i = 0; i <= static_cast<int>(ErrorCode::Internal); ++i) {
        auto c = static_cast<ErrorCode>(i);
        if (error_code_name(c) == name) return c;
    }
    return ErrorCode::Internal;
}

class DbError : public std::runtime_error {
public:
    DbError(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}
    ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

}  // namespace atlas
