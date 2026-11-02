#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace atlas {

enum class DataType : uint8_t {
    Null = 0,
    Integer = 1,
    Real = 2,
    Text = 3,
    Boolean = 4,
};

std::string_view type_name(DataType t);

// A single SQL value. NULL is represented by the empty (monostate) alternative.
class Value {
public:
    Value() = default;

    static Value null() { return Value(); }
    static Value integer(int64_t v);
    static Value real(double v);
    static Value text(std::string v);
    static Value boolean(bool v);

    DataType type() const;
    bool is_null() const { return data_.index() == 0; }
    bool is_numeric() const { return data_.index() == 1 || data_.index() == 2; }

    int64_t as_int() const;           // requires Integer
    double as_real() const;           // Integer or Real
    const std::string& as_text() const;  // requires Text
    bool as_bool() const;             // requires Boolean

    // Human readable rendering (NULL -> "NULL", text without quotes).
    std::string to_string() const;
    // Rendering as a SQL literal (text quoted and escaped).
    std::string to_sql() const;

    size_t hash() const;

    // Identity comparison used for grouping/DISTINCT/index keys: NULL == NULL.
    bool operator==(const Value& o) const;

private:
    std::variant<std::monostate, int64_t, double, std::string, bool> data_;
};

using Row = std::vector<Value>;

// Total order across all values: NULL < BOOLEAN < numbers < TEXT.
// Integers and reals compare numerically with each other.
int compare_values(const Value& a, const Value& b);

// Convert a value to the given column type, throwing DbError(Type) when the
// conversion would lose information or makes no sense.
Value cast_value(const Value& v, DataType target);

// Converts a C++ value to a SQL value: integers, floating point, bool,
// strings, nullptr (NULL) or a Value itself.
template <class T>
Value to_value(T&& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, Value>) return v;
    else if constexpr (std::is_same_v<U, std::nullptr_t>) return Value::null();
    else if constexpr (std::is_same_v<U, bool>) return Value::boolean(v);
    else if constexpr (std::is_integral_v<U>) return Value::integer(static_cast<int64_t>(v));
    else if constexpr (std::is_floating_point_v<U>) return Value::real(static_cast<double>(v));
    else if constexpr (std::is_convertible_v<T, std::string_view>) return Value::text(std::string(std::string_view(v)));
    else static_assert(sizeof(U) == 0, "unsupported parameter type");
}

// Builds a parameter list: atlas::params(42, "ana", 3.5, nullptr)
template <class... Ts>
std::vector<Value> params(Ts&&... vs) {
    return {to_value(std::forward<Ts>(vs))...};
}

struct ValueHash {
    size_t operator()(const Value& v) const { return v.hash(); }
};

struct RowHash {
    size_t operator()(const Row& r) const;
};

struct RowLess {
    bool operator()(const Row& a, const Row& b) const;
};

}  // namespace atlas
