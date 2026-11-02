#pragma once

#include <cstdint>
#include <vector>

#include "common/value.h"
#include "storage/table_heap.h"
#include "storage/wal.h"

namespace atlas {

// One logical change, kept so it can be reverted on ROLLBACK.
struct UndoEntry {
    enum class Kind { Insert, Delete, Update } kind;
    uint32_t table_id = 0;
    RID rid;      // location of the row before the change (Delete/Update) or of the new row (Insert)
    RID new_rid;  // Update: location after the change (may differ if the row moved)
    Row old_row;
    Row new_row;
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
