#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/error.h"
#include "common/result.h"
#include "net/socket.h"

// AtlasDB Wire Protocol (AWP), version 1.
//
// Every message is a frame:   [u8 type][u32 body length, big endian][body]
// Integers are big endian; a "string" is a u32 length followed by UTF-8 bytes;
// a "value" is a u8 type tag (0 NULL, 1 INTEGER i64, 2 REAL f64, 3 TEXT string,
// 4 BOOLEAN u8) followed by its payload.
//
// Client -> server
//   'H' Hello      : "AWP1" magic (4 bytes), u16 version, string client name
//   'Q' Query      : string sql (one or more ';'-separated statements)
//   'P' Prepare    : string name, string sql (exactly one statement, may use $n or ?)
//   'B' Execute    : string name, u16 nparams, nparams x value
//   'D' Close      : string name (forget a prepared statement)
//   'X' Terminate  : empty
//
// Server -> client
//   'O' HelloOk    : u16 version, string server name
//   'R' RowSet     : u16 ncols, ncols x (string name, u8 type), u64 nrows,
//                    nrows x ncols values, string tag
//   'C' Complete   : string tag, u64 rows affected
//   'p' Prepared   : u16 number of parameters the statement expects
//   'E' Error      : string code (e.g. "constraint_violation"), string message
//   'Z' Ready      : u8 transaction status ('I' idle, 'T' in transaction)
//
// After the handshake the client sends Query; the server answers with one
// RowSet/Complete per executed statement, stops at the first Error, and
// always finishes with Ready. Prepare answers Prepared (or Error), Execute
// answers RowSet/Complete (or Error), Close answers Complete; each is followed
// by Ready as well. Prepared statements belong to the connection. Parameter
// values travel as typed values and are never parsed as SQL.
namespace atlas::proto {

constexpr uint16_t VERSION = 1;
constexpr uint16_t DEFAULT_PORT = 5480;
constexpr uint32_t MAX_FRAME = 256u << 20;

enum class MsgType : uint8_t {
    Hello = 'H',
    Query = 'Q',
    Terminate = 'X',
    Prepare = 'P',
    Execute = 'B',
    Close = 'D',
    Prepared = 'p',
    HelloOk = 'O',
    RowSet = 'R',
    Complete = 'C',
    Error = 'E',
    Ready = 'Z',
};

struct Message {
    MsgType type;
    std::string body;
};

void write_message(net::Socket& s, MsgType type, std::string_view body);
// Returns nullopt when the peer closed the connection cleanly.
std::optional<Message> read_message(net::Socket& s);

std::string encode_hello(std::string_view client_name);
std::string decode_hello(std::string_view body);  // returns the client name
std::string encode_hello_ok(std::string_view server_name);
std::string decode_hello_ok(std::string_view body);

std::string encode_query(std::string_view sql);
std::string decode_query(std::string_view body);

std::string encode_prepare(std::string_view name, std::string_view sql);
std::pair<std::string, std::string> decode_prepare(std::string_view body);  // (name, sql)
std::string encode_prepared(size_t param_count);
size_t decode_prepared(std::string_view body);
std::string encode_execute(std::string_view name, const std::vector<Value>& params);
std::pair<std::string, std::vector<Value>> decode_execute(std::string_view body);
std::string encode_close(std::string_view name);
std::string decode_close(std::string_view body);

// Returns the message type (RowSet or Complete) and its body.
std::pair<MsgType, std::string> encode_result(const QueryResult& r);
QueryResult decode_rowset(std::string_view body);
QueryResult decode_complete(std::string_view body);

std::string encode_error(ErrorCode code, std::string_view message);
DbError decode_error(std::string_view body);

std::string encode_ready(bool in_transaction);
bool decode_ready(std::string_view body);

}  // namespace atlas::proto
