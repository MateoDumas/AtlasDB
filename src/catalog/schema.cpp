#include "catalog/schema.h"

namespace atlas {

std::string Column::type_sql() const {
    if (type == DataType::Text && max_length > 0) return "VARCHAR(" + std::to_string(max_length) + ")";
    return std::string(type_name(type));
}

int TableSchema::column_index(const std::string& n) const {
    for (size_t i = 0; i < columns.size(); ++i)
        if (columns[i].name == n) return static_cast<int>(i);
    return -1;
}

int TableSchema::primary_key_index() const {
    for (size_t i = 0; i < columns.size(); ++i)
        if (columns[i].primary_key) return static_cast<int>(i);
    return -1;
}

void TableSchema::serialize(ByteWriter& w) const {
    w.u32(id);
    w.str(name);
    w.u16(static_cast<uint16_t>(columns.size()));
    for (const auto& c : columns) {
        w.str(c.name);
        w.u8(static_cast<uint8_t>(c.type));
        w.u32(c.max_length);
        w.u8(static_cast<uint8_t>((c.not_null ? 1 : 0) | (c.primary_key ? 2 : 0) | (c.unique ? 4 : 0) |
                                  (c.foreign_key ? 8 : 0)));
        w.str(c.default_sql);
        if (c.foreign_key) {
            w.str(c.foreign_key->table);
            w.str(c.foreign_key->column);
        }
    }
    w.u16(static_cast<uint16_t>(checks.size()));
    for (const auto& c : checks) w.str(c);
}

TableSchema TableSchema::deserialize(ByteReader& r) {
    TableSchema s;
    s.id = r.u32();
    s.name = r.str();
    uint16_t n = r.u16();
    for (uint16_t i = 0; i < n; ++i) {
        Column c;
        c.name = r.str();
        c.type = static_cast<DataType>(r.u8());
        c.max_length = r.u32();
        uint8_t flags = r.u8();
        c.not_null = flags & 1;
        c.primary_key = flags & 2;
        c.unique = flags & 4;
        c.default_sql = r.str();
        if (flags & 8) {
            ForeignKey fk;
            fk.table = r.str();
            fk.column = r.str();
            c.foreign_key = fk;
        }
        s.columns.push_back(std::move(c));
    }
    uint16_t nc = r.u16();
    for (uint16_t i = 0; i < nc; ++i) s.checks.push_back(r.str());
    return s;
}

void IndexDef::serialize(ByteWriter& w) const {
    w.u32(id);
    w.str(name);
    w.str(table);
    w.str(column);
    w.u8(static_cast<uint8_t>((unique ? 1 : 0) | (implicit ? 2 : 0)));
}

IndexDef IndexDef::deserialize(ByteReader& r) {
    IndexDef d;
    d.id = r.u32();
    d.name = r.str();
    d.table = r.str();
    d.column = r.str();
    uint8_t f = r.u8();
    d.unique = f & 1;
    d.implicit = f & 2;
    return d;
}

}  // namespace atlas
