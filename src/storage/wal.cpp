#include "storage/wal.h"

#include "common/bytes.h"
#include "common/error.h"

namespace atlas {

namespace {
constexpr uint32_t MAX_RECORD_BODY = 64u * 1024 * 1024;
}

Wal::Wal(std::filesystem::path path) : file_(File::open(path)) { size_ = file_.size(); }

void Wal::append(const std::vector<LogRecord>& records, bool sync) {
    ByteWriter out;
    for (const auto& rec : records) {
        ByteWriter body;
        body.u64(rec.lsn);
        body.u8(static_cast<uint8_t>(rec.type));
        body.u64(rec.txn_id);
        body.u32(rec.table_id);
        body.u32(rec.rid.page);
        body.u16(rec.rid.slot);
        body.str(rec.payload);
        out.u32(static_cast<uint32_t>(body.size()));
        out.u32(crc32(body.data().data(), body.size()));
        out.raw(body.data().data(), body.size());
    }
    try {
        file_.write_at(size_, out.data().data(), out.size());
        if (sync) file_.sync();
    } catch (...) {
        // Cut off whatever part of the batch reached the file, so a later
        // recovery cannot mistake it for a committed transaction.
        try {
            file_.truncate(size_);
        } catch (...) {
        }
        throw;
    }
    size_ += out.size();
}

std::vector<LogRecord> Wal::read_all() {
    std::vector<LogRecord> out;
    uint64_t total = file_.size();
    std::string buf(total, '\0');
    if (total > 0) buf.resize(file_.read_at(0, buf.data(), total));

    size_t pos = 0;
    while (pos + 8 <= buf.size()) {
        ByteReader hdr(buf.data() + pos, 8);
        uint32_t len = hdr.u32();
        uint32_t crc = hdr.u32();
        if (len == 0 || len > MAX_RECORD_BODY || pos + 8 + len > buf.size()) break;
        const char* body = buf.data() + pos + 8;
        if (crc32(body, len) != crc) break;
        try {
            ByteReader r(body, len);
            LogRecord rec;
            rec.lsn = r.u64();
            rec.type = static_cast<LogType>(r.u8());
            rec.txn_id = r.u64();
            rec.table_id = r.u32();
            rec.rid.page = r.u32();
            rec.rid.slot = r.u16();
            rec.payload = r.str();
            out.push_back(std::move(rec));
        } catch (const DbError&) {
            break;
        }
        pos += 8 + len;
    }
    if (pos != buf.size()) {
        // Drop the torn tail so future appends start from a clean boundary.
        file_.truncate(pos);
        file_.sync();
    }
    size_ = pos;
    return out;
}

void Wal::reset() {
    file_.truncate(0);
    file_.sync();
    size_ = 0;
}

}  // namespace atlas
