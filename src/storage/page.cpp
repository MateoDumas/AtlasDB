#include "storage/page.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "common/bytes.h"
#include "common/error.h"

namespace atlas {

namespace {
constexpr uint8_t MAGIC[4] = {'A', 'T', 'P', '2'};
constexpr size_t CHECKSUM_OFFSET = 16;

uint32_t page_crc(const uint8_t* d) {
    uint32_t c = crc32(d, CHECKSUM_OFFSET);
    return crc32(d + CHECKSUM_OFFSET + 4, PAGE_SIZE - CHECKSUM_OFFSET - 4, c);
}
}

uint16_t SlottedPage::read16(size_t off) const {
    uint16_t v;
    std::memcpy(&v, d_ + off, 2);
    return v;
}

void SlottedPage::write16(size_t off, uint16_t v) { std::memcpy(d_ + off, &v, 2); }

void SlottedPage::init() {
    std::memset(d_, 0, PAGE_SIZE);
    write16(8, 0);
    write16(10, static_cast<uint16_t>(PAGE_SIZE));
    std::memcpy(d_ + 12, MAGIC, 4);
}

bool SlottedPage::is_initialized() const { return std::memcmp(d_ + 12, MAGIC, 4) == 0; }

bool SlottedPage::is_zeroed() const {
    return std::all_of(d_, d_ + PAGE_SIZE, [](uint8_t b) { return b == 0; });
}

void SlottedPage::update_checksum() {
    uint32_t c = page_crc(d_);
    std::memcpy(d_ + CHECKSUM_OFFSET, &c, 4);
}

bool SlottedPage::verify_checksum() const {
    uint32_t stored;
    std::memcpy(&stored, d_ + CHECKSUM_OFFSET, 4);
    return stored == page_crc(d_);
}

uint64_t SlottedPage::lsn() const {
    uint64_t v;
    std::memcpy(&v, d_, 8);
    return v;
}

void SlottedPage::set_lsn(uint64_t lsn) { std::memcpy(d_, &lsn, 8); }

uint16_t SlottedPage::slot_count() const { return read16(8); }

void SlottedPage::set_slot(uint16_t s, uint16_t off, uint16_t len) {
    write16(HEADER_SIZE + s * SLOT_SIZE, off);
    write16(HEADER_SIZE + s * SLOT_SIZE + 2, len);
}

std::optional<std::string_view> SlottedPage::get(uint16_t slot) const {
    if (slot >= slot_count()) return std::nullopt;
    uint16_t off = slot_offset(slot);
    if (off == 0) return std::nullopt;
    return std::string_view(reinterpret_cast<const char*>(d_ + off), slot_length(slot));
}

size_t SlottedPage::live_bytes() const {
    size_t n = 0;
    for (uint16_t s = 0; s < slot_count(); ++s)
        if (slot_offset(s) != 0) n += slot_length(s);
    return n;
}

size_t SlottedPage::total_free(uint16_t slots) const {
    size_t used = HEADER_SIZE + static_cast<size_t>(slots) * SLOT_SIZE + live_bytes();
    return used >= PAGE_SIZE ? 0 : PAGE_SIZE - used;
}

bool SlottedPage::has_empty_slot() const {
    for (uint16_t s = 0; s < slot_count(); ++s)
        if (slot_offset(s) == 0) return true;
    return false;
}

size_t SlottedPage::free_space() const {
    if (has_empty_slot()) return total_free(slot_count());
    size_t f = total_free(slot_count());
    return f > SLOT_SIZE ? f - SLOT_SIZE : 0;
}

void SlottedPage::compact() {
    struct Live {
        uint16_t slot;
        std::string bytes;
    };
    std::vector<Live> live;
    for (uint16_t s = 0; s < slot_count(); ++s) {
        if (slot_offset(s) != 0)
            live.push_back({s, std::string(reinterpret_cast<const char*>(d_ + slot_offset(s)), slot_length(s))});
    }
    size_t end = PAGE_SIZE;
    for (auto& l : live) {
        end -= l.bytes.size();
        std::memcpy(d_ + end, l.bytes.data(), l.bytes.size());
        set_slot(l.slot, static_cast<uint16_t>(end), static_cast<uint16_t>(l.bytes.size()));
    }
    write16(10, static_cast<uint16_t>(end));
}

// Writes rec into the contiguous gap, compacting first if necessary. The
// caller has verified the record fits in total free space.
void SlottedPage::write_record(uint16_t slot, std::string_view rec) {
    size_t dir_end = HEADER_SIZE + static_cast<size_t>(slot_count()) * SLOT_SIZE;
    if (data_start() < dir_end + rec.size()) compact();
    uint16_t start = static_cast<uint16_t>(data_start() - rec.size());
    if (start == 0) start = static_cast<uint16_t>(data_start());  // empty record: any nonzero offset
    std::memcpy(d_ + start, rec.data(), rec.size());
    if (!rec.empty()) write16(10, start);
    set_slot(slot, start, static_cast<uint16_t>(rec.size()));
}

std::optional<uint16_t> SlottedPage::insert(std::string_view rec) {
    if (rec.size() > MAX_RECORD) return std::nullopt;
    for (uint16_t s = 0; s < slot_count(); ++s) {
        if (slot_offset(s) == 0) {
            if (total_free(slot_count()) < rec.size()) return std::nullopt;
            write_record(s, rec);
            return s;
        }
    }
    uint16_t s = slot_count();
    if (total_free(static_cast<uint16_t>(s + 1)) < rec.size()) return std::nullopt;
    // Grow the directory; the new entry must not overlap the record area.
    size_t new_dir_end = HEADER_SIZE + static_cast<size_t>(s + 1) * SLOT_SIZE;
    if (data_start() < new_dir_end) compact();
    write16(8, static_cast<uint16_t>(s + 1));
    set_slot(s, 0, 0);
    write_record(s, rec);
    return s;
}

bool SlottedPage::put(uint16_t slot, std::string_view rec) {
    if (rec.size() > MAX_RECORD) return false;
    uint16_t count = std::max<uint16_t>(slot_count(), static_cast<uint16_t>(slot + 1));
    size_t existing = (slot < slot_count() && slot_offset(slot) != 0) ? slot_length(slot) : 0;
    if (total_free(count) + existing < rec.size()) return false;
    if (slot < slot_count() && slot_offset(slot) != 0) set_slot(slot, 0, 0);
    if (count > slot_count()) {
        size_t new_dir_end = HEADER_SIZE + static_cast<size_t>(count) * SLOT_SIZE;
        if (data_start() < new_dir_end) compact();
        for (uint16_t s = slot_count(); s < count; ++s) set_slot(s, 0, 0);
        write16(8, count);
    }
    write_record(slot, rec);
    return true;
}

bool SlottedPage::update(uint16_t slot, std::string_view rec) {
    if (slot >= slot_count() || slot_offset(slot) == 0)
        throw DbError(ErrorCode::Internal, "update of empty slot");
    uint16_t old_len = slot_length(slot);
    if (rec.size() <= old_len) {
        std::memcpy(d_ + slot_offset(slot), rec.data(), rec.size());
        set_slot(slot, slot_offset(slot), static_cast<uint16_t>(rec.size()));
        return true;
    }
    if (total_free(slot_count()) + old_len < rec.size()) return false;
    set_slot(slot, 0, 0);
    write_record(slot, rec);
    return true;
}

void SlottedPage::erase(uint16_t slot) {
    if (slot >= slot_count()) return;
    set_slot(slot, 0, 0);
    // Trim trailing empty slots so the directory does not grow forever.
    uint16_t n = slot_count();
    while (n > 0 && slot_offset(static_cast<uint16_t>(n - 1)) == 0) --n;
    write16(8, n);
    if (n == 0) write16(10, static_cast<uint16_t>(PAGE_SIZE));
}

}  // namespace atlas
