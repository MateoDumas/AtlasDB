#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/value.h"

namespace atlas {

uint32_t crc32(const void* data, size_t n, uint32_t seed = 0);

// Appends big-endian encoded primitives to a byte string.
class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void f64(double v);
    void str(std::string_view s);  // u32 length prefix + bytes
    void raw(const void* p, size_t n) { buf_.append(static_cast<const char*>(p), n); }
    void value(const Value& v);    // u8 type tag + payload
    void row(const Row& r);        // u16 count + values

    const std::string& data() const { return buf_; }
    std::string take() { return std::move(buf_); }
    size_t size() const { return buf_.size(); }

private:
    std::string buf_;
};

// Reads big-endian primitives, throwing DbError on truncated input.
class ByteReader {
public:
    ByteReader(const void* data, size_t n) : p_(static_cast<const uint8_t*>(data)), n_(n) {}
    explicit ByteReader(std::string_view s) : ByteReader(s.data(), s.size()) {}

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int64_t i64() { return static_cast<int64_t>(u64()); }
    double f64();
    std::string str();
    std::string_view raw(size_t n);
    Value value();
    Row row();

    bool done() const { return pos_ == n_; }
    size_t remaining() const { return n_ - pos_; }

private:
    void need(size_t k);
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

}  // namespace atlas
