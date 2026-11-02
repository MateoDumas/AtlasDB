#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/schema.h"
#include "engine/expr.h"
#include "index/index.h"
#include "storage/file.h"
#include "storage/table_heap.h"
#include "storage/wal.h"
#include "txn/lock.h"
#include "txn/transaction.h"

namespace atlas {

struct DatabaseOptions {
    size_t cache_pages_per_table = 2048;            // 8 MiB of clean pages per table
    size_t index_cache_nodes = 8192;                // decoded B+tree nodes kept per index
    uint64_t checkpoint_wal_bytes = 8ull << 20;     // checkpoint when the WAL grows past this
    bool sync_commits = true;                       // fsync the WAL on every commit
    std::chrono::milliseconds lock_timeout{5000};   // wait for the transaction lock
};

// A table: schema, heap storage, indexes and the parsed DEFAULT/CHECK clauses.
class Table {
public:
    Table(TableSchema schema, const std::filesystem::path& file, std::filesystem::path index_dir,
          size_t cache_pages);

    const TableSchema& schema() const { return schema_; }
    uint32_t id() const { return schema_.id; }
    const std::string& name() const { return schema_.name; }
    TableHeap& heap() { return heap_; }

    const std::vector<std::unique_ptr<Index>>& indexes() const { return indexes_; }
    Index* index_on(int column) const;  // prefers unique indexes
    Index* find_index(const std::string& name) const;
    // Opens (fresh = creates empty) the index file i<id>.idx and attaches it.
    Index& add_index(IndexDef def, bool fresh);
    void remove_index(const std::string& name);
    // Fills an empty index from the table's rows; throws on unique violations.
    void build_index(Index& idx);
    std::filesystem::path index_path(uint32_t index_id) const;

    const Expr* default_expr(size_t column) const { return defaults_[column].get(); }
    const std::vector<ExprPtr>& checks() const { return checks_; }

    std::optional<Row> read(RID rid);
    // Iterates rows in heap order starting at `from`; skips overflow chunks.
    bool next_row(RID from, RID& rid, Row& row);
    // Full row stored in a heap record: decodes it, following overflow chunks.
    // Returns nullopt for overflow chunk records (they are not rows).
    std::optional<Row> resolve(std::string_view record);

    // Row mutations that keep indexes in sync and report every physical heap
    // change made (for the WAL and for undo). No logging or constraint checks.
    RID insert_row(const Row& row, std::vector<HeapChange>& changes);
    void erase_row(RID rid, const Row& old_row, std::vector<HeapChange>& changes);
    RID update_row(RID rid, const Row& old_row, const Row& new_row, std::vector<HeapChange>& changes);
    // Restores the heap exactly as it was before `changes` (applied in reverse).
    void undo_heap(const std::vector<HeapChange>& changes);

    uint64_t row_count();

private:
    RID store(std::string bytes, std::vector<HeapChange>& changes);
    void release(RID rid, std::vector<HeapChange>& changes);

    TableSchema schema_;
    TableHeap heap_;
    std::vector<std::unique_ptr<Index>> indexes_;
    std::vector<ExprPtr> defaults_;
    std::vector<ExprPtr> checks_;
    std::filesystem::path index_dir_;
    size_t cache_pages_;
};

struct CreateTableStmt;
struct CreateIndexStmt;

class Database {
public:
    static std::unique_ptr<Database> open(const std::filesystem::path& dir, DatabaseOptions options = {});
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    const DatabaseOptions& options() const { return options_; }
    const std::filesystem::path& directory() const { return dir_; }

    // ---- catalog (callers hold the transaction lock) ----
    Table* find_table(const std::string& name);
    Table& table(const std::string& name);  // throws if missing
    Table* table_by_id(uint32_t id);
    std::vector<Table*> tables();
    std::pair<Table*, Index*> find_index(const std::string& name);

    void create_table(const CreateTableStmt& stmt);
    void drop_table(const std::string& name, bool if_exists);
    void create_index(const CreateIndexStmt& stmt);
    void drop_index(const std::string& name, bool if_exists);

    // ---- transactions ----
    TransactionLock& lock() { return lock_; }
    std::unique_ptr<Transaction> begin(bool explicit_txn);
    // Writes the transaction to the WAL and returns its commit LSN (0 if it
    // changed nothing). The commit is not yet durable: call wait_durable()
    // after releasing the lock, which lets concurrent commits share one fsync.
    uint64_t commit(Transaction& txn);
    // Blocks until the WAL is fsynced up to `lsn` (group commit). The first
    // waiter fsyncs on behalf of everyone; the others just wait for it.
    void wait_durable(uint64_t lsn);
    // LSN of the last commit written to the WAL (possibly not yet durable).
    uint64_t written_lsn() const { return written_lsn_.load(); }
    uint64_t wal_syncs() const { return wal_syncs_.load(); }
    void rollback(Transaction& txn, Transaction::Savepoint to = {0, 0});
    void checkpoint();

    // Index node cache: true when some index holds more nodes than its budget.
    bool index_cache_over_budget();
    // Evicts cold index nodes. The caller must hold the exclusive lock (so no
    // reader holds node pointers).
    void evict_index_nodes();
    // From a session that holds no lock (after a read found the cache over
    // budget): evicts only if the exclusive lock is free right now, never waiting.
    void try_evict_index_nodes(uint64_t session_id);

    // ---- row operations: enforce constraints, maintain indexes, log ----
    RID insert_row(Transaction& txn, Table& t, Row row);
    void delete_row(Transaction& txn, Table& t, RID rid, const Row& old_row);
    RID update_row(Transaction& txn, Table& t, RID rid, const Row& old_row, Row new_row);

    // Fills in defaults for absent columns and validates types/NOT NULL/CHECK.
    Row complete_row(Table& t, const std::vector<int>& provided, Row values);

    // Testing hook: drops all in-memory state without checkpointing, as if the
    // process had crashed. The object must not be used afterwards.
    void simulate_crash();

    uint64_t recovered_transactions() const { return recovered_txns_; }
    uint64_t last_lsn() const { return last_lsn_; }

private:
    Database(std::filesystem::path dir, DatabaseOptions options);

    std::filesystem::path table_path(uint32_t id) const;
    void load_catalog();
    std::string serialize_catalog() const;
    void recover();
    void apply_redo(const LogRecord& rec);
    void create_table_internal(TableSchema schema, const std::vector<IndexDef>& indexes, bool fresh_files);
    void replay_doublewrite();
    void remove_table(const std::string& name);
    void remove_orphan_files();
    void log_ddl(LogRecord rec);
    void check_wal_healthy() const;
    void publish_commit(uint64_t lsn);
    void maybe_checkpoint();

    Row validate_row(Table& t, Row row);
    void check_unique(Table& t, const Row& row, std::optional<RID> self);
    void check_foreign_keys(Table& t, const Row& row);
    void check_not_referenced(Table& parent, const Row& old_row, std::optional<RID> self, const Row* new_row);
    void log_changes(Transaction& txn, Table& t, std::vector<HeapChange>& changes);

    std::filesystem::path dir_;
    DatabaseOptions options_;
    FileLock dir_lock_;
    std::unique_ptr<Wal> wal_;
    std::map<std::string, std::unique_ptr<Table>> tables_;
    std::unordered_map<uint32_t, Table*> tables_by_id_;
    std::vector<std::filesystem::path> dropped_files_;  // deleted at the next checkpoint
    uint32_t next_table_id_ = 1;
    uint32_t next_index_id_ = 1;
    uint64_t last_lsn_ = 0;
    uint64_t checkpoint_lsn_ = 0;
    uint64_t next_txn_id_ = 1;
    uint64_t recovered_txns_ = 0;
    bool crashed_ = false;
    bool opened_ = false;
    TransactionLock lock_;
    std::mutex txn_id_mutex_;

    // Group commit state.
    std::mutex sync_mutex_;
    std::condition_variable sync_cv_;
    bool sync_in_progress_ = false;
    uint64_t durable_lsn_ = 0;               // guarded by sync_mutex_
    std::atomic<uint64_t> written_lsn_{0};
    std::atomic<uint64_t> wal_syncs_{0};
    std::atomic<bool> wal_failed_{false};    // an fsync failed: refuse further writes
};

}  // namespace atlas
