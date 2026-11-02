#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "storage/file.h"
#include "storage/table_heap.h"

namespace atlas {

enum class LogType : uint8_t {
    Put = 1,          // place row bytes at (table, rid)
    Erase = 2,        // remove row at (table, rid)
    CreateTable = 3,  // payload: serialized TableSchema
    DropTable = 4,    // table_id
    CreateIndex = 5,  // payload: serialized IndexDef
    DropIndex = 6,    // payload: index name
    Commit = 7,       // ends a transaction's batch
};

struct LogRecord {
    uint64_t lsn = 0;
    LogType type = LogType::Commit;
    uint64_t txn_id = 0;
    uint32_t table_id = 0;
    RID rid;
    std::string payload;
};

// Append-only write-ahead log.
//
// Frame format: [u32 body length][u32 crc32(body)][body], where body is
//   u64 lsn, u8 type, u64 txn, u32 table, u32 page, u16 slot, str payload.
// Only committed transactions are ever written (a whole batch per commit,
// followed by a single fsync), so the log is a pure redo log.
class Wal {
public:
    explicit Wal(std::filesystem::path path);

    // Appends the batch and makes it durable. LSNs must already be assigned.
    void append(const std::vector<LogRecord>& records, bool sync);

    // Reads every intact record. A torn or corrupt tail is truncated away.
    std::vector<LogRecord> read_all();

    void reset();
    uint64_t size() const { return size_; }

private:
    File file_;
    uint64_t size_ = 0;
};

}  // namespace atlas
