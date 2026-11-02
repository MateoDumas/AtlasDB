#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/value.h"
#include "storage/table_heap.h"
#include "storage/wal.h"

namespace atlas {

// One physical heap record change. `before` is the previous record (or none if
// the slot was empty); `after` the new one (or none if the record was erased).
// A row larger than a page spans several records (pointer + chunks), so one
// row operation produces several changes.
struct HeapChange {
    RID rid;
    std::optional<std::string> before;
    std::optional<std::string> after;
};

// One row change, kept so it can be reverted on ROLLBACK. The heap is restored
// byte for byte from `changes` (so every record returns to its exact slot,
// which the redo log relies on); the indexes are restored from the rows.
struct UndoEntry {
    enum class Kind { Insert, Delete, Update } kind;
    uint32_t table_id = 0;
    RID rid;      // location of the row before the change (Delete/Update) or of the new row (Insert)
    RID new_rid;  // Update: location after the change (may differ if the row moved)
    Row old_row;
    Row new_row;
    std::vector<HeapChange> changes;
};

// State of an in-flight transaction.
//
// Changes are applied to the in-memory pages immediately; `undo` records how
// to revert them, `redo` buffers the WAL records that are written only at
// COMMIT. Savepoints (positions in both vectors) give statement-level
// atomicity inside an explicit transaction.
struct Transaction {
    uint64_t id = 0;
    bool explicit_ = false;
    std::vector<UndoEntry> undo;
    std::vector<LogRecord> redo;

    struct Savepoint {
        size_t undo_size;
        size_t redo_size;
    };
    Savepoint savepoint() const { return {undo.size(), redo.size()}; }
};

}  // namespace atlas
