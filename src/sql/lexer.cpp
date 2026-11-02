#include "sql/lexer.h"

#include <cctype>

#include "common/error.h"

namespace atlas {

namespace {

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || (static_cast<unsigned char>(c) & 0x80);
}

bool is_ident_char(char c) { return is_ident_start(c) || std::isdigit(static_cast<unsigned char>(c)); }

class Lexer {
public:
    explicit Lexer(std::string_view s) : src_(s) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        while (true) {
            skip_space_and_comments();
            if (pos_ >= src_.size()) {
                out.push_back(Token{TokenKind::End, "", line_, col_});
                return out;
            }
            out.push_back(next());
        }
    }

private:
    [[noreturn]] void fail(const std::string& msg, int line, int col) {
        throw DbError(ErrorCode::Syntax,
                      msg + " at line " + std::to_string(line) + ", column " + std::to_string(col));
    }

    char peek(size_t off = 0) const { return pos_ + off < src_.size() ? src_[pos_ + off] : '\0'; }

    char advance() {
        char c = src_[pos_++];
        if (c == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        return c;
    }

    void skip_space_and_comments() {
        while (pos_ < src_.size()) {
            char c = peek();
            if (std::isspace(static_cast<unsigned char>(c))) {
                advance();
            } else if (c == '-' && peek(1) == '-') {
                while (pos_ < src_.size() && peek() != '\n') advance();
            } else if (c == '/' && peek(1) == '*') {
                int l = line_, cl = col_;
                advance();
                advance();
                while (!(peek() == '*' && peek(1) == '/')) {
                    if (pos_ >= src_.size()) fail("unterminated comment", l, cl);
                    advance();
                }
                advance();
                advance();
            } else {
                break;
            }
        }
    }

    Token next() {
        int l = line_, cl = col_;
        char c = peek();

        if (is_ident_start(c)) {
            std::string s;
            while (pos_ < src_.size() && is_ident_char(peek())) s += advance();
            return {TokenKind::Identifier, s, l, cl};
        }

        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
            std::string s;
            bool is_float = false;
            while (std::isdigit(static_cast<unsigned char>(peek()))) s += advance();
            if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
                is_float = true;
                s += advance();
                while (std::isdigit(static_cast<unsigned char>(peek()))) s += advance();
            } else if (peek() == '.' && !s.empty()) {
                // "1." is accepted as a float literal
                is_float = true;
                s += advance();
            }
            if (peek() == 'e' || peek() == 'E') {
                size_t save = pos_;
                int save_col = col_;
                std::string exp(1, advance());
                if (peek() == '+' || peek() == '-') exp += advance();
                if (std::isdigit(static_cast<unsigned char>(peek()))) {
                    while (std::isdigit(static_cast<unsigned char>(peek()))) exp += advance();
                    s += exp;
                    is_float = true;
                } else {
                    pos_ = save;
                    col_ = save_col;
                }
            }
            if (is_ident_start(peek())) fail("invalid numeric literal", l, cl);
            return {is_float ? TokenKind::Float : TokenKind::Integer, s, l, cl};
        }

        if (c == '\'') {
            advance();
            std::string s;
            while (true) {
                if (pos_ >= src_.size()) fail("unterminated string literal", l, cl);
                char ch = advance();
                if (ch == '\'') {
                    if (peek() == '\'') {
                        s += advance();
                        continue;
                    }
                    break;
                }
                s += ch;
            }
            return {TokenKind::String, s, l, cl};
        }

        if (c == '"') {
            advance();
            std::string s;
            while (true) {
                if (pos_ >= src_.size()) fail("unterminated quoted identifier", l, cl);
                char ch = advance();
                if (ch == '"') {
                    if (peek() == '"') {
                        s += advance();
                        continue;
                    }
                    break;
                }
                s += ch;
            }
            if (s.empty()) fail("empty quoted identifier", l, cl);
            return {TokenKind::QuotedIdentifier, s, l, cl};
        }

        static const char* two_char[] = {"<=", ">=", "<>", "!=", "||", "=="};
        for (const char* op : two_char) {
            if (c == op[0] && peek(1) == op[1]) {
                advance();
                advance();
                std::string s = op;
                if (s == "==") s = "=";
                if (s == "<>") s = "!=";
                return {TokenKind::Symbol, s, l, cl};
            }
        }

        if (c == '$' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            advance();
            std::string digits;
            while (std::isdigit(static_cast<unsigned char>(peek()))) digits += advance();
            if (digits.size() > 5 || std::stoi(digits) < 1) fail("invalid parameter number", l, cl);
            return {TokenKind::Parameter, digits, l, cl};
        }
        if (c == '?') {
            advance();
            return {TokenKind::Parameter, "", l, cl};
        }

        static const std::string_view singles = "(),;.*+-/%=<>";
        if (singles.find(c) != std::string_view::npos) {
            advance();
            return {TokenKind::Symbol, std::string(1, c), l, cl};
        }

        fail(std::string("unexpected character '") + c + "'", l, cl);
    }

    std::string_view src_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
};

}  // namespace

std::vector<Token> tokenize(std::string_view sql) { return Lexer(sql).run(); }

}  // namespace atlas
