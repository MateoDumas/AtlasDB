#include "engine/database.h"

#include <algorithm>
#include <map>
#include <set>
#include <cstring>

#include "common/bytes.h"
#include "common/error.h"
#include "sql/parser.h"
#include "storage/row_codec.h"

namespace atlas {

namespace {

constexpr char CATALOG_MAGIC[8] = {'A', 'T', 'L', 'A', 'S', 'C', 'A', 'T'};
constexpr uint32_t CATALOG_VERSION = 3;  // 2: page checksums; 3: persistent index files
constexpr char DOUBLEWRITE_MAGIC[4] = {'A', 'T', 'D', 'W'};
constexpr const char* DOUBLEWRITE_FILE = "doublewrite.buf";

[[noreturn]] void constraint(const std::string& msg) { throw DbError(ErrorCode::Constraint, msg); }

size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

bool types_compatible(DataType a, DataType b) {
    auto numeric = [](DataType t) { return t == DataType::Integer || t == DataType::Real; };
    return a == b || (numeric(a) && numeric(b));
}

}  // namespace

// ===========================================================================
// Table
// ===========================================================================

Table::Table(TableSchema schema, const std::filesystem::path& file, std::filesystem::path index_dir,
             size_t cache_pages)
    : schema_(std::move(schema)), heap_(file, cache_pages), index_dir_(std::move(index_dir)), cache_pages_(cache_pages) {
    Scope scope;
    scope.add_table(schema_.name, schema_);
    for (const auto& c : schema_.columns)
        defaults_.push_back(c.default_sql.empty() ? nullptr : parse_expression(c.default_sql));
    for (const auto& sql : schema_.checks) {
        auto e = parse_expression(sql);
        bind_expr(*e, scope);
        checks_.push_back(std::move(e));
    }
}

Index* Table::index_on(int column) const {
    Index* best = nullptr;
    for (const auto& idx : indexes_) {
        if (idx->column() != column) continue;
        if (idx->def().unique) return idx.get();
        if (!best) best = idx.get();
    }
    return best;
}

Index* Table::find_index(const std::string& name) const {
    for (const auto& idx : indexes_)
        if (idx->def().name == name) return idx.get();
    return nullptr;
}

std::filesystem::path Table::index_path(uint32_t index_id) const {
    return index_dir_ / ("i" + std::to_string(index_id) + ".idx");
}

Index& Table::add_index(IndexDef def, bool fresh) {
    int col = schema_.column_index(def.column);
    if (col < 0) throw DbError(ErrorCode::Semantic, "unknown column '" + def.column + "' in table '" + name() + "'");
    auto path = index_path(def.id);
    if (fresh) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    indexes_.push_back(std::make_unique<Index>(std::move(def), col, path, cache_pages_));
    return *indexes_.back();
}

void Table::build_index(Index& idx) {
    RID rid{0, 0};
    Row row;
    while (next_row(rid, rid, row)) {
        const Value& v = row[idx.column()];
        if (idx.def().unique && !v.is_null() && idx.find_other(v))
            constraint("cannot create unique index '" + idx.def().name + "': duplicate value " + v.to_sql());
        idx.insert(v, rid);
        if (++rid.slot == 0) ++rid.page;  // advance past the current record
    }
}

void Table::remove_index(const std::string& name) {
    std::erase_if(indexes_, [&](const auto& idx) { return idx->def().name == name; });
}

// ---- overflow records ------------------------------------------------------
//
// Rows up to INLINE_LIMIT bytes are stored as a single heap record. Larger rows
// become a chain of chunk records plus a small pointer record at the row's RID:
//   pointer: FF 01 | u32 total length | u32 page, u16 slot of the first chunk
//   chunk:   FF 02 | u32 page, u16 slot of the next chunk (END = none) | data
// An inline row starts with its u16 column count, which never begins with 0xFF.

namespace {
constexpr size_t INLINE_LIMIT = 2000;
constexpr size_t MAX_ROW_BYTES = 16u << 20;
constexpr size_t CHUNK_HEADER = 8;
constexpr size_t CHUNK_DATA = SlottedPage::MAX_RECORD - CHUNK_HEADER;
constexpr RID END_OF_CHAIN{0xFFFFFFFFu, 0xFFFF};

bool is_pointer(std::string_view r) { return r.size() >= 2 && uint8_t(r[0]) == 0xFF && r[1] == 0x01; }
bool is_chunk(std::string_view r) { return r.size() >= 2 && uint8_t(r[0]) == 0xFF && r[1] == 0x02; }

std::string make_link(uint8_t kind, uint32_t length, RID rid, std::string_view data = {}) {
    ByteWriter w;
    w.u8(0xFF);
    w.u8(kind);
    if (kind == 0x01) w.u32(length);
    w.u32(rid.page);
    w.u16(rid.slot);
    w.raw(data.data(), data.size());
    return w.take();
}

RID read_link(std::string_view r, size_t offset) {
    ByteReader rd(r.substr(offset, 6));
    RID rid;
    rid.page = rd.u32();
    rid.slot = rd.u16();
    return rid;
}
}  // namespace

std::optional<Row> Table::resolve(std::string_view record) {
    if (is_chunk(record)) return std::nullopt;
    if (!is_pointer(record)) return decode_row(record);
    if (record.size() < 12) throw DbError(ErrorCode::Corruption, "truncated overflow pointer");
    ByteReader rd(record.substr(2, 4));
    uint32_t total = rd.u32();
    std::string bytes;
    bytes.reserve(total);
    for (RID next = read_link(record, 6); next != END_OF_CHAIN;) {
        auto chunk = heap_.get(next);
        if (!chunk || !is_chunk(*chunk) || bytes.size() > total)
            throw DbError(ErrorCode::Corruption, "broken overflow chain at " + next.to_string());
        bytes.append(*chunk, CHUNK_HEADER);
        next = read_link(*chunk, 2);
    }
    if (bytes.size() != total) throw DbError(ErrorCode::Corruption, "overflow row has the wrong length");
    return decode_row(bytes);
}

std::optional<Row> Table::read(RID rid) {
    auto bytes = heap_.get(rid);
    if (!bytes) return std::nullopt;
    return resolve(*bytes);
}

bool Table::next_row(RID from, RID& rid, Row& row) {
    std::string rec;
    while (heap_.next(from, rid, rec)) {
        if (auto r = resolve(rec)) {
            row = std::move(*r);
            return true;
        }
        from = rid;
        if (++from.slot == 0) ++from.page;
    }
    return false;
}

// Writes a row's bytes (inline, or as chunks + pointer) and returns its RID.
RID Table::store(std::string bytes, std::vector<HeapChange>& changes) {
    if (bytes.size() > MAX_ROW_BYTES)
        throw DbError(ErrorCode::Constraint, "row too large (" + std::to_string(bytes.size()) + " bytes, max " +
                                                 std::to_string(MAX_ROW_BYTES) + ")");
    if (bytes.size() <= INLINE_LIMIT) {
        RID rid = heap_.insert(bytes);
        changes.push_back({rid, std::nullopt, std::move(bytes)});
        return rid;
    }
    // Write chunks back to front so each one can point at its successor.
    size_t count = (bytes.size() + CHUNK_DATA - 1) / CHUNK_DATA;
    RID next = END_OF_CHAIN;
    for (size_t i = count; i-- > 0;) {
        size_t off = i * CHUNK_DATA;
        std::string chunk =
            make_link(0x02, 0, next, std::string_view(bytes).substr(off, std::min(CHUNK_DATA, bytes.size() - off)));
        next = heap_.insert(chunk);
        changes.push_back({next, std::nullopt, std::move(chunk)});
    }
    std::string pointer = make_link(0x01, static_cast<uint32_t>(bytes.size()), next);
    RID rid = heap_.insert(pointer);
    changes.push_back({rid, std::nullopt, std::move(pointer)});
    return rid;
}

// Erases a row's record and, for overflow rows, its chunks.
void Table::release(RID rid, std::vector<HeapChange>& changes) {
    auto main = heap_.get(rid);
    if (!main) return;
    heap_.erase(rid);
    std::string record = std::move(*main);
    RID next = is_pointer(record) ? read_link(record, 6) : END_OF_CHAIN;
    changes.push_back({rid, std::move(record), std::nullopt});
    while (next != END_OF_CHAIN) {
        auto chunk = heap_.get(next);
        if (!chunk || !is_chunk(*chunk)) throw DbError(ErrorCode::Corruption, "broken overflow chain");
        RID following = read_link(*chunk, 2);
        heap_.erase(next);
        changes.push_back({next, std::move(*chunk), std::nullopt});
        next = following;
    }
}

void Table::undo_heap(const std::vector<HeapChange>& changes) {
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        if (it->before) heap_.put(it->rid, *it->before);
        else heap_.erase(it->rid);
    }
}

RID Table::insert_row(const Row& row, std::vector<HeapChange>& changes) {
    RID rid = store(encode_row(row), changes);
    for (auto& idx : indexes_) idx->insert(row[idx->column()], rid);
    return rid;
}

void Table::erase_row(RID rid, const Row& old_row, std::vector<HeapChange>& changes) {
    release(rid, changes);
    for (auto& idx : indexes_) idx->erase(old_row[idx->column()], rid);
}

RID Table::update_row(RID rid, const Row& old_row, const Row& new_row, std::vector<HeapChange>& changes) {
    std::string bytes = encode_row(new_row);
    RID out = rid;
    auto current = heap_.get(rid);
    if (!current) throw DbError(ErrorCode::Internal, "update of a missing row at " + rid.to_string());
    if (!is_pointer(*current) && bytes.size() <= INLINE_LIMIT && heap_.update(rid, bytes)) {
        changes.push_back({rid, std::move(*current), std::move(bytes)});
    } else {
        release(rid, changes);
        out = store(std::move(bytes), changes);
    }
    for (auto& idx : indexes_) {
        const Value& before = old_row[idx->column()];
        const Value& after = new_row[idx->column()];
        if (out != rid || !(before == after) || before.type() != after.type()) {
            idx->erase(before, rid);
            idx->insert(after, out);
        }
    }
    return out;
}

uint64_t Table::row_count() {
    // Every index has exactly one entry per row (NULL values included).
    if (!indexes_.empty()) return indexes_.front()->size();
    uint64_t n = 0;
    RID rid{0, 0};
    Row row;
    while (next_row(rid, rid, row)) {
        ++n;
        if (++rid.slot == 0) ++rid.page;
    }
    return n;
}

// ===========================================================================
// Database: lifecycle, catalog, recovery
// ===========================================================================

Database::Database(std::filesystem::path dir, DatabaseOptions options)
    : dir_(std::move(dir)), options_(options), dir_lock_(FileLock::acquire(dir_ / "LOCK")) {}

std::unique_ptr<Database> Database::open(const std::filesystem::path& dir, DatabaseOptions options) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) throw DbError(ErrorCode::IO, "cannot create directory '" + dir.string() + "': " + ec.message());
    std::unique_ptr<Database> db(new Database(dir, options));
    db->replay_doublewrite();
    db->load_catalog();
    db->wal_ = std::make_unique<Wal>(dir / "wal.log");
    db->recover();
    db->opened_ = true;
    return db;
}

Database::~Database() {
    // A database that failed to open (or "crashed") must not checkpoint: that
    // would persist a partially loaded catalog and discard the WAL.
    if (crashed_ || !opened_) return;
    try {
        checkpoint();
    } catch (...) {
        // Nothing sensible to do in a destructor; the WAL still holds the data.
    }
}

void Database::simulate_crash() { crashed_ = true; }

std::filesystem::path Database::table_path(uint32_t id) const {
    return dir_ / ("t" + std::to_string(id) + ".tbl");
}

void Database::load_catalog() {
    auto path = dir_ / "catalog.bin";
    if (!std::filesystem::exists(path)) return;
    File f = File::open(path);
    std::string buf(f.size(), '\0');
    buf.resize(f.read_at(0, buf.data(), buf.size()));
    if (buf.size() < 12 || std::memcmp(buf.data(), CATALOG_MAGIC, 8) != 0)
        throw DbError(ErrorCode::Corruption, "catalog file is not an AtlasDB catalog");
    ByteReader crc_reader(buf.data() + buf.size() - 4, 4);
    if (crc32(buf.data(), buf.size() - 4) != crc_reader.u32())
        throw DbError(ErrorCode::Corruption, "catalog checksum mismatch");

    ByteReader r(buf.data() + 8, buf.size() - 12);
    uint32_t version = r.u32();
    if (version != CATALOG_VERSION)
        throw DbError(ErrorCode::Corruption, "database format version " + std::to_string(version) +
                                                 " is not supported (this build reads version " +
                                                 std::to_string(CATALOG_VERSION) + ")");
    checkpoint_lsn_ = r.u64();
    last_lsn_ = checkpoint_lsn_;
    next_table_id_ = r.u32();
    next_index_id_ = r.u32();
    uint32_t ntables = r.u32();
    for (uint32_t i = 0; i < ntables; ++i) create_table_internal(TableSchema::deserialize(r), {}, false);
    uint32_t nindexes = r.u32();
    for (uint32_t i = 0; i < nindexes; ++i) {
        IndexDef def = IndexDef::deserialize(r);
        table(def.table).add_index(def, false);
    }
}

std::string Database::serialize_catalog() const {
    ByteWriter w;
    w.raw(CATALOG_MAGIC, 8);
    w.u32(CATALOG_VERSION);
    w.u64(checkpoint_lsn_);
    w.u32(next_table_id_);
    w.u32(next_index_id_);
    w.u32(static_cast<uint32_t>(tables_.size()));
    for (const auto& [name, t] : tables_) t->schema().serialize(w);
    std::vector<const IndexDef*> defs;
    for (const auto& [name, t] : tables_)
        for (const auto& idx : t->indexes()) defs.push_back(&idx->def());
    w.u32(static_cast<uint32_t>(defs.size()));
    for (const auto* d : defs) d->serialize(w);
    uint32_t crc = crc32(w.data().data(), w.size());
    w.u32(crc);
    return w.take();
}

void Database::recover() {
    std::vector<LogRecord> records = wal_->read_all();
    std::vector<const LogRecord*> pending;
    for (const auto& rec : records) {
        last_lsn_ = std::max(last_lsn_, rec.lsn);
        if (rec.type != LogType::Commit) {
            pending.push_back(&rec);
            continue;
        }
        if (rec.lsn > checkpoint_lsn_) {
            for (const LogRecord* p : pending) apply_redo(*p);
            ++recovered_txns_;
        }
        pending.clear();
    }
    // Records left in `pending` belong to a batch whose COMMIT never reached
    // the disk; they are ignored and erased by the checkpoint below.

    // Indexes are persistent and were kept in sync by apply_redo: nothing to rebuild.
    if (wal_->size() > 0 || !dropped_files_.empty()) checkpoint();
    remove_orphan_files();
}

// Deletes table and index files that the catalog no longer references
// (e.g. dropped right before a crash, or left by a failed CREATE).
void Database::remove_orphan_files() {
    std::set<std::string> live;
    for (auto& [name, t] : tables_) {
        live.insert(table_path(t->id()).filename().string());
        for (const auto& idx : t->indexes()) live.insert(t->index_path(idx->def().id).filename().string());
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
        std::string fname = entry.path().filename().string();
        auto ext = entry.path().extension();
        if ((ext == ".tbl" || ext == ".idx") && !live.count(fname)) {
            std::error_code ec;
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

// A complete doublewrite buffer means a checkpoint crashed while writing pages
// in place: copy every page (and the catalog) from the buffer so that all files
// are exactly at that checkpoint. An incomplete buffer means the crash happened
// before any data file was touched, so it is simply discarded.
void Database::replay_doublewrite() {
    auto path = dir_ / DOUBLEWRITE_FILE;
    if (!std::filesystem::exists(path)) return;
    std::string buf;
    {
        File f = File::open(path);
        buf.resize(f.size());
        buf.resize(f.read_at(0, buf.data(), buf.size()));
    }
    bool valid = buf.size() >= 8 && std::memcmp(buf.data(), DOUBLEWRITE_MAGIC, 4) == 0;
    if (valid) {
        ByteReader crc_reader(buf.data() + buf.size() - 4, 4);
        valid = crc32(buf.data(), buf.size() - 4) == crc_reader.u32();
    }
    if (valid) {
        ByteReader r(buf.data() + 4, buf.size() - 8);
        r.u64();  // checkpoint LSN (also recorded inside the catalog)
        uint32_t n = r.u32();
        std::map<std::string, File> files;
        for (uint32_t i = 0; i < n; ++i) {
            std::string name = r.str();
            uint32_t page = r.u32();
            auto data = r.raw(PAGE_SIZE);
            auto it = files.find(name);
            if (it == files.end()) it = files.emplace(name, File::open(dir_ / name)).first;
            it->second.write_at(static_cast<uint64_t>(page) * PAGE_SIZE, data.data(), PAGE_SIZE);
        }
        for (auto& [name, f] : files) f.sync();
        write_file_atomic(dir_ / "catalog.bin", r.str());
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void Database::apply_redo(const LogRecord& rec) {
    switch (rec.type) {
        case LogType::Put:
        case LogType::Erase: {
            Table* t = table_by_id(rec.table_id);
            if (!t) return;
            auto& heap = t->heap();
            if (heap.page_lsn_for_redo(rec.rid.page) >= rec.lsn) return;  // already on disk
            // Keep the persistent indexes in step with the heap. Overflow chunks are
            // written before their pointer and erased after it, so resolving a row
            // here always finds its whole chain.
            if (auto old = heap.get(rec.rid)) {
                if (auto old_row = t->resolve(*old))
                    for (const auto& idx : t->indexes()) idx->erase((*old_row)[idx->column()], rec.rid);
            }
            if (rec.type == LogType::Put) {
                heap.put(rec.rid, rec.payload);
                if (auto row = t->resolve(rec.payload))
                    for (const auto& idx : t->indexes()) idx->insert((*row)[idx->column()], rec.rid);
            } else {
                heap.erase(rec.rid);
            }
            heap.set_page_lsn(rec.rid.page, rec.lsn);
            return;
        }
        case LogType::CreateTable: {
            ByteReader r(rec.payload);
            TableSchema schema = TableSchema::deserialize(r);
            std::vector<IndexDef> defs;
            for (uint16_t n = r.u16(); n > 0; --n) {
                defs.push_back(IndexDef::deserialize(r));
                next_index_id_ = std::max(next_index_id_, defs.back().id + 1);
            }
            next_table_id_ = std::max(next_table_id_, schema.id + 1);
            create_table_internal(std::move(schema), defs, true);
            return;
        }
        case LogType::DropTable: {
            if (Table* t = table_by_id(rec.table_id)) {
                dropped_files_.push_back(table_path(t->id()));
                for (const auto& idx : t->indexes()) dropped_files_.push_back(t->index_path(idx->def().id));
                remove_table(t->name());
            }
            return;
        }
        case LogType::CreateIndex: {
            ByteReader r(rec.payload);
            IndexDef def = IndexDef::deserialize(r);
            next_index_id_ = std::max(next_index_id_, def.id + 1);
            if (Table* t = find_table(def.table); t && !t->find_index(def.name)) t->build_index(t->add_index(def, true));
            return;
        }
        case LogType::DropIndex: {
            auto [t, idx] = find_index(rec.payload);
            if (t) {
                dropped_files_.push_back(t->index_path(idx->def().id));
                t->remove_index(rec.payload);
            }
            return;
        }
        case LogType::Commit: return;
    }
}

// Checkpoint protocol (always runs with no transaction in flight):
//   1. serialize dirty index nodes into their pages;
//   2. write every dirty page plus the new catalog to the doublewrite buffer, fsync;
//   3. write the pages in place and fsync the data files;
//   4. replace the catalog atomically, empty the WAL, delete the buffer.
// A crash during 3 is repaired from the buffer at the next open, so data and
// index files are always exactly at some checkpoint when recovery starts.
void Database::checkpoint() {
    if (crashed_) return;
    for (auto& [name, t] : tables_)
        for (const auto& idx : t->indexes()) idx->persist();

    std::vector<std::pair<std::string, Pager*>> pagers;
    for (auto& [name, t] : tables_) {
        pagers.emplace_back(table_path(t->id()).filename().string(), &t->heap().pager());
        for (const auto& idx : t->indexes())
            pagers.emplace_back(t->index_path(idx->def().id).filename().string(), &idx->pager());
    }

    // Nothing changed since the last checkpoint: avoid needless fsyncs.
    bool any_dirty = false;
    for (auto& [file, pager] : pagers) any_dirty = any_dirty || pager->dirty_pages() > 0;
    if (!any_dirty && wal_->size() == 0 && dropped_files_.empty() && checkpoint_lsn_ == last_lsn_ &&
        std::filesystem::exists(dir_ / "catalog.bin"))
        return;

    checkpoint_lsn_ = last_lsn_;
    std::string catalog = serialize_catalog();

    ByteWriter dw;
    dw.raw(DOUBLEWRITE_MAGIC, 4);
    dw.u64(checkpoint_lsn_);
    uint32_t count = 0;
    ByteWriter pages;
    for (auto& [file, pager] : pagers) {
        for (auto& [id, data] : pager->dirty_copies()) {
            pages.str(file);
            pages.u32(id);
            pages.raw(data.data(), PAGE_SIZE);
            ++count;
        }
    }
    dw.u32(count);
    dw.raw(pages.data().data(), pages.size());
    dw.str(catalog);
    dw.u32(crc32(dw.data().data(), dw.size()));
    {
        File f = File::open(dir_ / DOUBLEWRITE_FILE);
        f.truncate(0);
        f.write_at(0, dw.data().data(), dw.size());
        f.sync();
    }

    for (auto& [file, pager] : pagers) pager->flush();
    write_file_atomic(dir_ / "catalog.bin", catalog);
    {
        // Never truncate the WAL under a group-commit fsync. Every change is now in
        // the fsynced data files, so all commits written so far are durable.
        std::unique_lock lk(sync_mutex_);
        sync_cv_.wait(lk, [&] { return !sync_in_progress_; });
        wal_->reset();
        durable_lsn_ = std::max(durable_lsn_, last_lsn_);
        written_lsn_ = std::max(written_lsn_.load(), last_lsn_);
        sync_cv_.notify_all();
    }
    std::error_code ec;
    std::filesystem::remove(dir_ / DOUBLEWRITE_FILE, ec);
    for (const auto& path : dropped_files_) std::filesystem::remove(path, ec);
    dropped_files_.clear();
}

void Database::maybe_checkpoint() {
    if (wal_->size() >= options_.checkpoint_wal_bytes) checkpoint();
}

// ===========================================================================
// Catalog access & DDL
// ===========================================================================

Table* Database::find_table(const std::string& name) {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : it->second.get();
}

Table& Database::table(const std::string& name) {
    Table* t = find_table(name);
    if (!t) throw DbError(ErrorCode::Semantic, "table '" + name + "' does not exist");
    return *t;
}

Table* Database::table_by_id(uint32_t id) {
    auto it = tables_by_id_.find(id);
    return it == tables_by_id_.end() ? nullptr : it->second;
}

void Database::remove_table(const std::string& name) {
    if (Table* t = find_table(name)) tables_by_id_.erase(t->id());
    tables_.erase(name);
}

std::vector<Table*> Database::tables() {
    std::vector<Table*> out;
    for (auto& [name, t] : tables_) out.push_back(t.get());
    return out;
}

std::pair<Table*, Index*> Database::find_index(const std::string& name) {
    for (auto& [tname, t] : tables_)
        if (Index* idx = t->find_index(name)) return {t.get(), idx};
    return {nullptr, nullptr};
}

void Database::create_table_internal(TableSchema schema, const std::vector<IndexDef>& indexes, bool fresh_files) {
    auto path = table_path(schema.id);
    if (fresh_files) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    auto t = std::make_unique<Table>(std::move(schema), path, dir_, options_.cache_pages_per_table);
    for (const auto& def : indexes) t->add_index(def, fresh_files);
    std::string name = t->name();
    tables_by_id_[t->id()] = t.get();
    tables_[name] = std::move(t);
}

void Database::log_ddl(LogRecord rec) {
    uint64_t txn = 0;
    {
        std::lock_guard lk(txn_id_mutex_);
        txn = next_txn_id_++;
    }
    rec.lsn = ++last_lsn_;
    rec.txn_id = txn;
    LogRecord commit;
    commit.type = LogType::Commit;
    commit.lsn = ++last_lsn_;
    commit.txn_id = txn;
    check_wal_healthy();
    wal_->append({rec, commit}, /*sync=*/false);
    publish_commit(commit.lsn);
    wait_durable(commit.lsn);  // DDL is rare: just wait while holding the lock
    maybe_checkpoint();
}

void Database::create_table(const CreateTableStmt& stmt) {
    if (find_table(stmt.name)) {
        if (stmt.if_not_exists) return;
        throw DbError(ErrorCode::Semantic, "table '" + stmt.name + "' already exists");
    }
    if (stmt.columns.empty()) throw DbError(ErrorCode::Semantic, "a table needs at least one column");
    if (stmt.columns.size() > 1000) throw DbError(ErrorCode::Semantic, "a table can have at most 1000 columns");

    TableSchema schema;
    schema.name = stmt.name;
    for (const auto& cd : stmt.columns) {
        if (schema.column_index(cd.name) >= 0)
            throw DbError(ErrorCode::Semantic, "duplicate column name '" + cd.name + "'");
        Column c;
        c.name = cd.name;
        c.type = cd.type;
        c.max_length = cd.max_length;
        c.not_null = cd.not_null;
        c.primary_key = cd.primary_key;
        c.unique = cd.unique;
        if (cd.references) c.foreign_key = ForeignKey{cd.references->table, cd.references->column};
        if (cd.default_value) {
            if (contains_columns(*cd.default_value) || contains_aggregate(*cd.default_value))
                throw DbError(ErrorCode::Semantic, "DEFAULT for column '" + c.name + "' must be a constant");
            Value v = cast_value(eval_expr(*cd.default_value, {}), c.type);
            (void)v;
            c.default_sql = cd.default_value->to_string();
        }
        schema.columns.push_back(std::move(c));
    }

    auto column_ref = [&](const std::string& col, const char* what) -> Column& {
        int i = schema.column_index(col);
        if (i < 0) throw DbError(ErrorCode::Semantic, std::string(what) + " references unknown column '" + col + "'");
        return schema.columns[static_cast<size_t>(i)];
    };
    for (const auto& col : stmt.primary_key) column_ref(col, "PRIMARY KEY").primary_key = true;
    for (const auto& col : stmt.unique) column_ref(col, "UNIQUE").unique = true;
    for (const auto& [col, ref] : stmt.foreign_keys)
        column_ref(col, "FOREIGN KEY").foreign_key = ForeignKey{ref.table, ref.column};

    int pk_count = 0;
    for (auto& c : schema.columns) {
        if (c.primary_key) {
            ++pk_count;
            c.not_null = true;
            c.unique = false;  // the primary key index already enforces uniqueness
        }
    }
    if (pk_count > 1) throw DbError(ErrorCode::Semantic, "a table can have only one PRIMARY KEY column");

    // Validate foreign keys.
    for (auto& c : schema.columns) {
        if (!c.foreign_key) continue;
        auto& fk = *c.foreign_key;
        const TableSchema* parent = nullptr;
        if (fk.table == schema.name) parent = &schema;
        else if (Table* p = find_table(fk.table)) parent = &p->schema();
        if (!parent) throw DbError(ErrorCode::Semantic, "referenced table '" + fk.table + "' does not exist");
        if (fk.column.empty()) {
            int pk = parent->primary_key_index();
            if (pk < 0) throw DbError(ErrorCode::Semantic, "referenced table '" + fk.table + "' has no primary key");
            fk.column = parent->columns[static_cast<size_t>(pk)].name;
        }
        int pc = parent->column_index(fk.column);
        if (pc < 0)
            throw DbError(ErrorCode::Semantic, "referenced column '" + fk.table + "." + fk.column + "' does not exist");
        const Column& pcol = parent->columns[static_cast<size_t>(pc)];
        if (!pcol.primary_key && !pcol.unique)
            throw DbError(ErrorCode::Semantic, "referenced column '" + fk.table + "." + fk.column +
                                                   "' must be a PRIMARY KEY or UNIQUE");
        if (!types_compatible(pcol.type, c.type))
            throw DbError(ErrorCode::Semantic, "foreign key column '" + c.name + "' type does not match '" +
                                                   fk.table + "." + fk.column + "'");
    }

    // Validate CHECK constraints by binding them against the new table.
    Scope scope;
    scope.add_table(schema.name, schema);
    auto add_check = [&](const Expr& e) {
        if (contains_aggregate(e)) throw DbError(ErrorCode::Semantic, "aggregates are not allowed in CHECK");
        auto copy = e.clone();
        bind_expr(*copy, scope);
        schema.checks.push_back(e.to_string());
    };
    for (const auto& cd : stmt.columns)
        if (cd.check) add_check(*cd.check);
    for (const auto& chk : stmt.checks) add_check(*chk);

    schema.id = next_table_id_++;
    std::vector<IndexDef> indexes;
    for (const auto& c : schema.columns) {
        if (c.primary_key)
            indexes.push_back(IndexDef{next_index_id_++, schema.name + "_pkey", schema.name, c.name, true, true});
        else if (c.unique)
            indexes.push_back(
                IndexDef{next_index_id_++, schema.name + "_" + c.name + "_key", schema.name, c.name, true, true});
    }
    ByteWriter w;
    schema.serialize(w);
    w.u16(static_cast<uint16_t>(indexes.size()));
    for (const auto& d : indexes) d.serialize(w);
    std::string name = schema.name;
    uint32_t id = schema.id;
    create_table_internal(std::move(schema), indexes, true);

    LogRecord rec;
    rec.type = LogType::CreateTable;
    rec.table_id = id;
    rec.payload = w.take();
    try {
        log_ddl(std::move(rec));
    } catch (...) {
        remove_table(name);
        throw;
    }
}

void Database::drop_table(const std::string& name, bool if_exists) {
    Table* t = find_table(name);
    if (!t) {
        if (if_exists) return;
        throw DbError(ErrorCode::Semantic, "table '" + name + "' does not exist");
    }
    for (auto& [other_name, other] : tables_) {
        if (other_name == name) continue;
        for (const auto& c : other->schema().columns)
            if (c.foreign_key && c.foreign_key->table == name)
                throw DbError(ErrorCode::Constraint,
                              "cannot drop table '" + name + "': it is referenced by '" + other_name + "'");
    }
    LogRecord rec;
    rec.type = LogType::DropTable;
    rec.table_id = t->id();
    log_ddl(std::move(rec));
    dropped_files_.push_back(table_path(t->id()));
    for (const auto& idx : t->indexes()) dropped_files_.push_back(t->index_path(idx->def().id));
    remove_table(name);
}

void Database::create_index(const CreateIndexStmt& stmt) {
    if (find_index(stmt.name).second) {
        if (stmt.if_not_exists) return;
        throw DbError(ErrorCode::Semantic, "index '" + stmt.name + "' already exists");
    }
    Table& t = table(stmt.table);
    if (t.schema().column_index(stmt.column) < 0)
        throw DbError(ErrorCode::Semantic, "unknown column '" + stmt.column + "' in table '" + t.name() + "'");
    IndexDef def{next_index_id_++, stmt.name, stmt.table, stmt.column, stmt.unique, false};
    Index& idx = t.add_index(def, true);
    try {
        t.build_index(idx);
        ByteWriter w;
        def.serialize(w);
        LogRecord lr;
        lr.type = LogType::CreateIndex;
        lr.table_id = t.id();
        lr.payload = w.take();
        log_ddl(std::move(lr));
    } catch (...) {
        t.remove_index(def.name);
        std::error_code ec;
        std::filesystem::remove(t.index_path(def.id), ec);
        throw;
    }
}

void Database::drop_index(const std::string& name, bool if_exists) {
    auto [t, idx] = find_index(name);
    if (!idx) {
        if (if_exists) return;
        throw DbError(ErrorCode::Semantic, "index '" + name + "' does not exist");
    }
    if (idx->def().implicit)
        throw DbError(ErrorCode::Semantic, "index '" + name + "' enforces a constraint and cannot be dropped");
    LogRecord rec;
    rec.type = LogType::DropIndex;
    rec.table_id = t->id();
    rec.payload = name;
    log_ddl(std::move(rec));
    dropped_files_.push_back(t->index_path(idx->def().id));
    t->remove_index(name);
}

// ===========================================================================
// Transactions
// ===========================================================================

std::unique_ptr<Transaction> Database::begin(bool explicit_txn) {
    auto txn = std::make_unique<Transaction>();
    {
        std::lock_guard lk(txn_id_mutex_);
        txn->id = next_txn_id_++;
    }
    txn->explicit_ = explicit_txn;
    return txn;
}

void Database::check_wal_healthy() const {
    if (wal_failed_)
        throw DbError(ErrorCode::IO, "a WAL fsync failed earlier; restart the database to recover safely");
}

void Database::wait_durable(uint64_t lsn) {
    if (lsn == 0) return;
    std::unique_lock lk(sync_mutex_);
    while (durable_lsn_ < lsn) {
        check_wal_healthy();
        if (sync_in_progress_) {
            sync_cv_.wait(lk);
            continue;
        }
        // Become the leader: one fsync covers every commit written so far.
        sync_in_progress_ = true;
        uint64_t target = written_lsn_.load();
        lk.unlock();
        bool ok = true;
        try {
            wal_->sync();
        } catch (...) {
            ok = false;
        }
        lk.lock();
        sync_in_progress_ = false;
        if (ok) {
            durable_lsn_ = std::max(durable_lsn_, target);
            ++wal_syncs_;
        } else {
            wal_failed_ = true;
        }
        sync_cv_.notify_all();
    }
}

uint64_t Database::commit(Transaction& txn) {
    if (txn.redo.empty()) {
        txn.undo.clear();
        return 0;
    }
    check_wal_healthy();
    uint64_t first_lsn = last_lsn_;
    for (auto& rec : txn.redo) {
        rec.lsn = ++last_lsn_;
        rec.txn_id = txn.id;
    }
    LogRecord commit_rec;
    commit_rec.type = LogType::Commit;
    commit_rec.lsn = ++last_lsn_;
    commit_rec.txn_id = txn.id;
    txn.redo.push_back(commit_rec);
    try {
        wal_->append(txn.redo, /*sync=*/false);
    } catch (...) {
        last_lsn_ = first_lsn;
        txn.redo.pop_back();
        rollback(txn);
        throw;
    }
    // Stamp pages so a later recovery knows these changes are already applied.
    for (const auto& rec : txn.redo) {
        if (rec.type != LogType::Put && rec.type != LogType::Erase) continue;
        if (Table* t = table_by_id(rec.table_id)) t->heap().set_page_lsn(rec.rid.page, rec.lsn);
    }
    txn.undo.clear();
    txn.redo.clear();
    publish_commit(commit_rec.lsn);
    maybe_checkpoint();
    return commit_rec.lsn;
}

// Makes a commit written to the WAL visible to wait_durable().
void Database::publish_commit(uint64_t lsn) {
    written_lsn_ = lsn;
    if (!options_.sync_commits) {
        // No fsync at all: "durable" as soon as it is written.
        std::lock_guard lk(sync_mutex_);
        durable_lsn_ = std::max(durable_lsn_, lsn);
    }
}

void Database::rollback(Transaction& txn, Transaction::Savepoint to) {
    while (txn.undo.size() > to.undo_size) {
        UndoEntry u = std::move(txn.undo.back());
        txn.undo.pop_back();
        Table* t = table_by_id(u.table_id);
        if (!t) continue;
        for (const auto& idx : t->indexes()) {
            if (u.kind != UndoEntry::Kind::Delete) idx->erase(u.new_row[idx->column()], u.new_rid);
            if (u.kind != UndoEntry::Kind::Insert) idx->insert(u.old_row[idx->column()], u.rid);
        }
        t->undo_heap(u.changes);
    }
    txn.redo.resize(std::min(txn.redo.size(), to.redo_size));
}

// ===========================================================================
// Constraint enforcement & row operations
// ===========================================================================

Row Database::complete_row(Table& t, const std::vector<int>& provided, Row values) {
    const auto& cols = t.schema().columns;
    Row row(cols.size());
    std::vector<bool> set(cols.size(), false);
    for (size_t i = 0; i < provided.size(); ++i) {
        auto c = static_cast<size_t>(provided[i]);
        row[c] = std::move(values[i]);
        set[c] = true;
    }
    for (size_t c = 0; c < cols.size(); ++c)
        if (!set[c] && t.default_expr(c)) row[c] = eval_expr(*t.default_expr(c), {});
    return row;
}

Row Database::validate_row(Table& t, Row row) {
    const auto& cols = t.schema().columns;
    if (row.size() != cols.size()) throw DbError(ErrorCode::Internal, "row width does not match table");
    for (size_t i = 0; i < cols.size(); ++i) {
        const Column& c = cols[i];
        try {
            row[i] = cast_value(row[i], c.type);
        } catch (const DbError& e) {
            throw DbError(ErrorCode::Type, "column '" + c.name + "': " + e.what());
        }
        if (row[i].is_null() && c.not_null)
            constraint("NULL value in column '" + t.name() + "." + c.name + "' violates NOT NULL constraint");
        if (c.max_length && row[i].type() == DataType::Text && utf8_length(row[i].as_text()) > c.max_length)
            constraint("value too long for column '" + t.name() + "." + c.name + "' (max " +
                       std::to_string(c.max_length) + " characters)");
    }
    for (size_t i = 0; i < t.checks().size(); ++i) {
        Value v = eval_expr(*t.checks()[i], row);
        if (v.is_null()) continue;
        if (v.type() != DataType::Boolean) throw DbError(ErrorCode::Type, "CHECK constraint must be boolean");
        if (!v.as_bool()) constraint("CHECK constraint " + t.schema().checks[i] + " failed on table '" + t.name() + "'");
    }
    return row;
}

void Database::check_unique(Table& t, const Row& row, std::optional<RID> self) {
    for (const auto& idx : t.indexes()) {
        if (!idx->def().unique) continue;
        const Value& v = row[idx->column()];
        if (v.is_null()) continue;
        if (idx->find_other(v, self))
            constraint("duplicate key value violates unique constraint '" + idx->def().name + "' (" +
                       idx->def().column + " = " + v.to_sql() + ")");
    }
}

void Database::check_foreign_keys(Table& t, const Row& row) {
    const auto& cols = t.schema().columns;
    for (size_t i = 0; i < cols.size(); ++i) {
        if (!cols[i].foreign_key || row[i].is_null()) continue;
        const auto& fk = *cols[i].foreign_key;
        Table& parent = table(fk.table);
        int pc = parent.schema().column_index(fk.column);
        if (&parent == &t && row[static_cast<size_t>(pc)] == row[i]) continue;  // row references itself
        Index* idx = parent.index_on(pc);
        bool found = idx ? !idx->lookup(row[i]).empty() : false;
        if (!found)
            constraint("foreign key violation: " + t.name() + "." + cols[i].name + " = " + row[i].to_sql() +
                       " has no matching row in '" + fk.table + "." + fk.column + "'");
    }
}

void Database::check_not_referenced(Table& parent, const Row& old_row, std::optional<RID> self, const Row* new_row) {
    for (auto& [cname, child] : tables_) {
        const auto& ccols = child->schema().columns;
        for (size_t j = 0; j < ccols.size(); ++j) {
            if (!ccols[j].foreign_key || ccols[j].foreign_key->table != parent.name()) continue;
            int pc = parent.schema().column_index(ccols[j].foreign_key->column);
            const Value& v = old_row[static_cast<size_t>(pc)];
            if (v.is_null()) continue;
            if (new_row && (*new_row)[static_cast<size_t>(pc)] == v) continue;

            auto is_self = [&](RID rid) { return child.get() == &parent && self && rid == *self; };
            bool referenced = false;
            if (Index* idx = child->index_on(static_cast<int>(j))) {
                for (RID rid : idx->lookup(v))
                    if (!is_self(rid)) referenced = true;
            } else {
                RID rid{0, 0};
                Row r;
                while (!referenced && child->next_row(rid, rid, r)) {
                    if (!r[j].is_null() && r[j] == v && !is_self(rid)) referenced = true;
                    if (++rid.slot == 0) ++rid.page;
                }
            }
            if (referenced)
                constraint("foreign key violation: " + parent.name() + "." + ccols[j].foreign_key->column + " = " +
                           v.to_sql() + " is still referenced from '" + cname + "." + ccols[j].name + "'");
        }
    }
}

// Turns physical heap changes into redo records. The new record bytes move into
// the log (undo only needs the before-images), so each row is encoded once.
void Database::log_changes(Transaction& txn, Table& t, std::vector<HeapChange>& changes) {
    for (auto& c : changes) {
        LogRecord rec;
        rec.type = c.after ? LogType::Put : LogType::Erase;
        rec.table_id = t.id();
        rec.rid = c.rid;
        if (c.after) rec.payload = std::move(*c.after);
        c.after.reset();
        txn.redo.push_back(std::move(rec));
    }
}

RID Database::insert_row(Transaction& txn, Table& t, Row row) {
    row = validate_row(t, std::move(row));
    check_unique(t, row, std::nullopt);
    check_foreign_keys(t, row);
    std::vector<HeapChange> changes;
    RID rid = t.insert_row(row, changes);
    log_changes(txn, t, changes);
    txn.undo.push_back(UndoEntry{UndoEntry::Kind::Insert, t.id(), rid, rid, {}, std::move(row), std::move(changes)});
    return rid;
}

void Database::delete_row(Transaction& txn, Table& t, RID rid, const Row& old_row) {
    check_not_referenced(t, old_row, rid, nullptr);
    std::vector<HeapChange> changes;
    t.erase_row(rid, old_row, changes);
    log_changes(txn, t, changes);
    txn.undo.push_back(UndoEntry{UndoEntry::Kind::Delete, t.id(), rid, rid, old_row, {}, std::move(changes)});
}

RID Database::update_row(Transaction& txn, Table& t, RID rid, const Row& old_row, Row new_row) {
    new_row = validate_row(t, std::move(new_row));
    check_unique(t, new_row, rid);
    check_foreign_keys(t, new_row);
    check_not_referenced(t, old_row, rid, &new_row);
    std::vector<HeapChange> changes;
    RID new_rid = t.update_row(rid, old_row, new_row, changes);
    log_changes(txn, t, changes);
    txn.undo.push_back(
        UndoEntry{UndoEntry::Kind::Update, t.id(), rid, new_rid, old_row, std::move(new_row), std::move(changes)});
    return new_rid;
}

}  // namespace atlas
