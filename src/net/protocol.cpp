#include "net/protocol.h"

#include <cstring>

#include "common/bytes.h"

namespace atlas::proto {

namespace {
constexpr char MAGIC[4] = {'A', 'W', 'P', '1'};

[[noreturn]] void bad(const std::string& msg) { throw DbError(ErrorCode::Protocol, msg); }

template <class F>
auto decoding(F&& f) {
    try {
        return f();
    } catch (const DbError& e) {
        if (e.code() == ErrorCode::Corruption) bad(std::string("malformed message: ") + e.what());
        throw;
    }
}
}  // namespace

void write_message(net::Socket& s, MsgType type, std::string_view body) {
    if (body.size() > MAX_FRAME) bad("message too large");
    ByteWriter w;
    w.u8(static_cast<uint8_t>(type));
    w.u32(static_cast<uint32_t>(body.size()));
    w.raw(body.data(), body.size());
    s.send_all(w.data().data(), w.size());
}

std::optional<Message> read_message(net::Socket& s) {
    uint8_t hdr[5];
    if (!s.recv_exact(hdr, sizeof hdr)) return std::nullopt;
    ByteReader r(hdr, sizeof hdr);
    Message m;
    m.type = static_cast<MsgType>(r.u8());
    uint32_t len = r.u32();
    if (len > MAX_FRAME) bad("frame of " + std::to_string(len) + " bytes exceeds the limit");
    m.body.resize(len);
    if (len > 0 && !s.recv_exact(m.body.data(), len)) bad("connection closed in the middle of a message");
    return m;
}

std::string encode_hello(std::string_view client_name) {
    ByteWriter w;
    w.raw(MAGIC, 4);
    w.u16(VERSION);
    w.str(client_name);
    return w.take();
}

std::string decode_hello(std::string_view body) {
    return decoding([&] {
        ByteReader r(body);
        auto magic = r.raw(4);
        if (std::memcmp(magic.data(), MAGIC, 4) != 0) bad("not an AtlasDB client (bad magic)");
        uint16_t v = r.u16();
        if (v != VERSION) bad("unsupported protocol version " + std::to_string(v));
        return r.str();
    });
}

std::string encode_hello_ok(std::string_view server_name) {
    ByteWriter w;
    w.u16(VERSION);
    w.str(server_name);
    return w.take();
}

std::string decode_hello_ok(std::string_view body) {
    return decoding([&] {
        ByteReader r(body);
        uint16_t v = r.u16();
        if (v != VERSION) bad("server speaks protocol version " + std::to_string(v));
        return r.str();
    });
}

std::string encode_query(std::string_view sql) {
    ByteWriter w;
    w.str(sql);
    return w.take();
}

std::string decode_query(std::string_view body) {
    return decoding([&] {
        ByteReader r(body);
        return r.str();
    });
}

std::string encode_prepare(std::string_view name, std::string_view sql) {
    ByteWriter w;
    w.str(name);
    w.str(sql);
    return w.take();
}

std::pair<std::string, std::string> decode_prepare(std::string_view body) {
    return decoding([&] {
        ByteReader r(body);
        std::string name = r.str();
        std::string sql = r.str();
        return std::pair{std::move(name), std::move(sql)};
    });
}

std::string encode_prepared(size_t param_count) {
    ByteWriter w;
    w.u16(static_cast<uint16_t>(param_count));
    return w.take();
}

size_t decode_prepared(std::string_view body) {
    return decoding([&] {
        ByteReader r(body);
        return static_cast<size_t>(r.u16());
    });
}

std::string encode_execute(std::string_view name, const std::vector<Value>& params) {
    if (params.size() > 0xFFFF) bad("too many parameters");
    ByteWriter w;
    w.str(name);
    w.u16(static_cast<uint16_t>(params.size()));
    for (const auto& v : params) w.value(v);
    return w.take();
}

std::pair<std::string, std::vector<Value>> decode_execute(std::string_view body) {
    return decoding([&] {
        ByteReader r(body);
        std::string name = r.str();
        uint16_t n = r.u16();
        std::vector<Value> params;
        params.reserve(n);
        for (uint16_t i = 0; i < n; ++i) params.push_back(r.value());
        return std::pair{std::move(name), std::move(params)};
    });
}

std::string encode_close(std::string_view name) { return encode_query(name); }

std::string decode_close(std::string_view body) { return decode_query(body); }

std::pair<MsgType, std::string> encode_result(const QueryResult& res) {
    ByteWriter w;
    if (!res.has_rows) {
        w.str(res.tag);
        w.u64(res.affected);
        return {MsgType::Complete, w.take()};
    }
    w.u16(static_cast<uint16_t>(res.columns.size()));
    for (size_t i = 0; i < res.columns.size(); ++i) {
        w.str(res.columns[i]);
        w.u8(static_cast<uint8_t>(i < res.types.size() ? res.types[i] : DataType::Null));
    }
    w.u64(res.rows.size());
    for (const auto& row : res.rows)
        for (size_t i = 0; i < res.columns.size(); ++i) w.value(i < row.size() ? row[i] : Value::null());
    w.str(res.tag);
    return {MsgType::RowSet, w.take()};
}

QueryResult decode_rowset(std::string_view body) {
    return decoding([&] {
        ByteReader r(body);
        QueryResult res;
        res.has_rows = true;
        uint16_t ncols = r.u16();
        for (uint16_t i = 0; i < ncols; ++i) {
            res.columns.push_back(r.str());
            res.types.push_back(static_cast<DataType>(r.u8()));
        }
        uint64_t nrows = r.u64();
        // Every value takes at least one byte, which bounds a hostile row count.
        // A result set without columns cannot have rows at all.
        if (ncols == 0 ? nrows > 0 : nrows > r.remaining() / ncols) bad("row count exceeds message size");
        res.rows.reserve(static_cast<size_t>(nrows));
        for (uint64_t i = 0; i < nrows; ++i) {
            Row row;
            row.reserve(ncols);
            for (uint16_t c = 0; c < ncols; ++c) row.push_back(r.value());
            res.rows.push_back(std::move(row));
        }
        res.tag = r.str();
        res.affected = nrows;
        return res;
    });
}

QueryResult decode_complete(std::string_view body) {
    return decoding([&] {
        ByteReader r(body);
        QueryResult res;
        res.tag = r.str();
        res.affected = r.u64();
        return res;
    });
}

std::string encode_error(ErrorCode code, std::string_view message) {
    ByteWriter w;
    w.str(error_code_name(code));
    w.str(message);
    return w.take();
}

DbError decode_error(std::string_view body) {
    return decoding([&] {
        ByteReader r(body);
        std::string code = r.str();
        std::string msg = r.str();
        return DbError(error_code_from_name(code), msg);
    });
}

std::string encode_ready(bool in_transaction) { return std::string(1, in_transaction ? 'T' : 'I'); }

bool decode_ready(std::string_view body) {
    if (body.size() != 1) bad("malformed Ready message");
    return body[0] == 'T';
}

}  // namespace atlas::proto
