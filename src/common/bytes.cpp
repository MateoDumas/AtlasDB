#include "common/bytes.h"

#include <array>
#include <cstring>

#include "common/error.h"

namespace atlas {

namespace {
std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
}  // namespace

uint32_t crc32(const void* data, size_t n, uint32_t seed) {
    static const auto table = make_crc_table();
    auto p = static_cast<const uint8_t*>(data);
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void ByteWriter::u16(uint16_t v) {
    u8(static_cast<uint8_t>(v >> 8));
    u8(static_cast<uint8_t>(v));
}

void ByteWriter::u32(uint32_t v) {
    u16(static_cast<uint16_t>(v >> 16));
    u16(static_cast<uint16_t>(v));
}

void ByteWriter::u64(uint64_t v) {
    u32(static_cast<uint32_t>(v >> 32));
    u32(static_cast<uint32_t>(v));
}

void ByteWriter::f64(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    u64(bits);
}

void ByteWriter::str(std::string_view s) {
    u32(static_cast<uint32_t>(s.size()));
    buf_.append(s.data(), s.size());
}

void ByteWriter::value(const Value& v) {
    u8(static_cast<uint8_t>(v.type()));
    switch (v.type()) {
        case DataType::Null: break;
        case DataType::Integer: i64(v.as_int()); break;
        case DataType::Real: f64(v.as_real()); break;
        case DataType::Text: str(v.as_text()); break;
        case DataType::Boolean: u8(v.as_bool() ? 1 : 0); break;
    }
}

void ByteWriter::row(const Row& r) {
    u16(static_cast<uint16_t>(r.size()));
    for (const auto& v : r) value(v);
}

void ByteReader::need(size_t k) {
    if (n_ - pos_ < k) throw DbError(ErrorCode::Corruption, "unexpected end of encoded data");
}

uint8_t ByteReader::u8() {
    need(1);
    return p_[pos_++];
}

uint16_t ByteReader::u16() {
    uint16_t hi = u8();
    return static_cast<uint16_t>((hi << 8) | u8());
}

uint32_t ByteReader::u32() {
    uint32_t hi = u16();
    return (hi << 16) | u16();
}

uint64_t ByteReader::u64() {
    uint64_t hi = u32();
    return (hi << 32) | u32();
}

double ByteReader::f64() {
    uint64_t bits = u64();
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

std::string ByteReader::str() {
    uint32_t n = u32();
    return std::string(raw(n));
}

std::string_view ByteReader::raw(size_t n) {
    need(n);
    std::string_view s(reinterpret_cast<const char*>(p_ + pos_), n);
    pos_ += n;
    return s;
}

Value ByteReader::value() {
    auto tag = static_cast<DataType>(u8());
    switch (tag) {
        case DataType::Null: return Value::null();
        case DataType::Integer: return Value::integer(i64());
        case DataType::Real: return Value::real(f64());
        case DataType::Text: return Value::text(str());
        case DataType::Boolean: return Value::boolean(u8() != 0);
    }
    throw DbError(ErrorCode::Corruption, "unknown value tag " + std::to_string(static_cast<int>(tag)));
}

Row ByteReader::row() {
    uint16_t n = u16();
    Row r;
    r.reserve(n);
    for (uint16_t i = 0; i < n; ++i) r.push_back(value());
    return r;
}

}  // namespace atlas
