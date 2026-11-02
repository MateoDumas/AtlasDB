#include "common/value.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

#include "common/error.h"

namespace atlas {

std::string_view type_name(DataType t) {
    switch (t) {
        case DataType::Null: return "NULL";
        case DataType::Integer: return "INTEGER";
        case DataType::Real: return "REAL";
        case DataType::Text: return "TEXT";
        case DataType::Boolean: return "BOOLEAN";
    }
    return "UNKNOWN";
}

Value Value::integer(int64_t v) {
    Value r;
    r.data_ = v;
    return r;
}

Value Value::real(double v) {
    Value r;
    r.data_ = v;
    return r;
}

Value Value::text(std::string v) {
    Value r;
    r.data_ = std::move(v);
    return r;
}

Value Value::boolean(bool v) {
    Value r;
    r.data_ = v;
    return r;
}

DataType Value::type() const {
    switch (data_.index()) {
        case 1: return DataType::Integer;
        case 2: return DataType::Real;
        case 3: return DataType::Text;
        case 4: return DataType::Boolean;
        default: return DataType::Null;
    }
}

int64_t Value::as_int() const {
    if (auto p = std::get_if<int64_t>(&data_)) return *p;
    throw DbError(ErrorCode::Type, "expected INTEGER, got " + std::string(type_name(type())));
}

double Value::as_real() const {
    if (auto p = std::get_if<double>(&data_)) return *p;
    if (auto p = std::get_if<int64_t>(&data_)) return static_cast<double>(*p);
    throw DbError(ErrorCode::Type, "expected a number, got " + std::string(type_name(type())));
}

const std::string& Value::as_text() const {
    if (auto p = std::get_if<std::string>(&data_)) return *p;
    throw DbError(ErrorCode::Type, "expected TEXT, got " + std::string(type_name(type())));
}

bool Value::as_bool() const {
    if (auto p = std::get_if<bool>(&data_)) return *p;
    throw DbError(ErrorCode::Type, "expected BOOLEAN, got " + std::string(type_name(type())));
}

static std::string format_real(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.15g", d);
    std::string s = buf;
    // Make reals visibly distinct from integers.
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

std::string Value::to_string() const {
    switch (data_.index()) {
        case 1: return std::to_string(std::get<int64_t>(data_));
        case 2: return format_real(std::get<double>(data_));
        case 3: return std::get<std::string>(data_);
        case 4: return std::get<bool>(data_) ? "true" : "false";
        default: return "NULL";
    }
}

std::string Value::to_sql() const {
    if (data_.index() != 3) return to_string();
    std::string out = "'";
    for (char c : std::get<std::string>(data_)) {
        if (c == '\'') out += '\'';
        out += c;
    }
    out += '\'';
    return out;
}

size_t Value::hash() const {
    switch (data_.index()) {
        case 1: return std::hash<int64_t>{}(std::get<int64_t>(data_));
        case 2: {
            double d = std::get<double>(data_);
            // Integral reals must hash like the equal integer.
            if (std::trunc(d) == d && d >= -9.2e18 && d <= 9.2e18)
                return std::hash<int64_t>{}(static_cast<int64_t>(d));
            return std::hash<double>{}(d);
        }
        case 3: return std::hash<std::string>{}(std::get<std::string>(data_));
        case 4: return std::get<bool>(data_) ? 0x9e3779b9u : 0x7f4a7c15u;
        default: return 0x12345678u;
    }
}

bool Value::operator==(const Value& o) const { return compare_values(*this, o) == 0; }

static int type_rank(const Value& v) {
    switch (v.type()) {
        case DataType::Null: return 0;
        case DataType::Boolean: return 1;
        case DataType::Integer:
        case DataType::Real: return 2;
        case DataType::Text: return 3;
    }
    return 4;
}

int compare_values(const Value& a, const Value& b) {
    int ra = type_rank(a), rb = type_rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (a.type()) {
        case DataType::Null: return 0;
        case DataType::Boolean: return static_cast<int>(a.as_bool()) - static_cast<int>(b.as_bool());
        case DataType::Text: {
            int c = a.as_text().compare(b.as_text());
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        default: break;
    }
    if (a.type() == DataType::Integer && b.type() == DataType::Integer) {
        int64_t x = a.as_int(), y = b.as_int();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    double x = a.as_real(), y = b.as_real();
    return x < y ? -1 : (x > y ? 1 : 0);
}

Value cast_value(const Value& v, DataType target) {
    if (v.is_null() || v.type() == target) return v;
    switch (target) {
        case DataType::Real:
            if (v.type() == DataType::Integer) return Value::real(static_cast<double>(v.as_int()));
            break;
        case DataType::Integer:
            if (v.type() == DataType::Real) {
                double d = v.as_real();
                if (std::trunc(d) == d && d >= -9.2e18 && d <= 9.2e18)
                    return Value::integer(static_cast<int64_t>(d));
                throw DbError(ErrorCode::Type, "cannot store non-integral value " + v.to_string() +
                                                   " in INTEGER column");
            }
            break;
        default: break;
    }
    throw DbError(ErrorCode::Type, "cannot convert " + std::string(type_name(v.type())) + " value " +
                                       v.to_sql() + " to " + std::string(type_name(target)));
}

size_t RowHash::operator()(const Row& r) const {
    size_t h = 0xcbf29ce484222325ull;
    for (const auto& v : r) h = (h ^ v.hash()) * 0x100000001b3ull;
    return h;
}

bool RowLess::operator()(const Row& a, const Row& b) const {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        int c = compare_values(a[i], b[i]);
        if (c != 0) return c < 0;
    }
    return a.size() < b.size();
}

}  // namespace atlas
