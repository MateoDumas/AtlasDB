#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/bytes.h"
#include "common/value.h"

namespace atlas {

struct ForeignKey {
    std::string table;
    std::string column;
};

struct Column {
    std::string name;
    DataType type = DataType::Text;
    uint32_t max_length = 0;  // VARCHAR(n); 0 = unlimited
    bool not_null = false;
    bool primary_key = false;
    bool unique = false;
    std::string default_sql;  // empty = no default (NULL)
    std::optional<ForeignKey> foreign_key;

    std::string type_sql() const;
};

struct TableSchema {
    uint32_t id = 0;
    std::string name;
    std::vector<Column> columns;
    std::vector<std::string> checks;  // CHECK constraints as SQL text

    int column_index(const std::string& name) const;
    int primary_key_index() const;

    void serialize(ByteWriter& w) const;
    static TableSchema deserialize(ByteReader& r);
};

struct IndexDef {
    uint32_t id = 0;  // names the index file i<id>.idx
    std::string name;
    std::string table;
    std::string column;
    bool unique = false;
    bool implicit = false;  // created automatically for PRIMARY KEY / UNIQUE

    void serialize(ByteWriter& w) const;
    static IndexDef deserialize(ByteReader& r);
};

}  // namespace atlas
