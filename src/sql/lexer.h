#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace atlas {

enum class TokenKind {
    Identifier,        // bare word, also used for keywords
    QuotedIdentifier,  // "name" -- never treated as a keyword
    Integer,
    Float,
    String,            // 'text'
    Symbol,            // operators and punctuation
    Parameter,         // $1, $2, ... (text = number) or ? (text empty)
    End,
};

struct Token {
    TokenKind kind;
    std::string text;  // identifier/symbol text, or decoded string literal
    int line = 1;
    int column = 1;
};

// Splits SQL text into tokens. Throws DbError(Syntax) on malformed input.
std::vector<Token> tokenize(std::string_view sql);

}  // namespace atlas
