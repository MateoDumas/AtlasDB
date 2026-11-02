#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace atlas {

constexpr size_t PAGE_SIZE = 4096;

// Slotted page layout:
//
//   [0..8)   page LSN (last WAL record applied to this page)
//   [8..10)  slot count
//   [10..12) start of the record area (records grow downwards from the end)
//   [12..16) magic "ATP2"
//   [16..20) CRC32 of the rest of the page, set when the page is written to disk
//   [20..)   slot directory: per slot {u16 offset, u16 length}; offset 0 = empty
//
// Record bytes live at the end of the page. Erased records leave holes that are
// reclaimed lazily by compact().
class SlottedPage {
public:
    static constexpr size_t HEADER_SIZE = 20;
    static constexpr size_t SLOT_SIZE = 4;
    static constexpr size_t MAX_RECORD = PAGE_SIZE - HEADER_SIZE - SLOT_SIZE;

    explicit SlottedPage(uint8_t* data) : d_(data) {}

    void init();
    bool is_initialized() const;
    bool is_zeroed() const;

    // Checksums detect torn or corrupted pages when they are read back from disk.
    void update_checksum();
    bool verify_checksum() const;

    uint64_t lsn() const;
    void set_lsn(uint64_t lsn);
    uint16_t slot_count() const;

    std::optional<std::string_view> get(uint16_t slot) const;
    // Inserts into any free slot; returns nullopt when the record does not fit.
    std::optional<uint16_t> insert(std::string_view rec);
    // Places a record in a specific slot (growing the directory as needed).
    // Returns false if it does not fit.
    bool put(uint16_t slot, std::string_view rec);
    // Replaces the record in `slot`; returns false (leaving it untouched) if it does not fit.
    bool update(uint16_t slot, std::string_view rec);
    void erase(uint16_t slot);

    // Bytes available for a new record (accounts for a possibly needed new slot).
    size_t free_space() const;
    void compact();

private:
    uint16_t read16(size_t off) const;
    void write16(size_t off, uint16_t v);
    uint16_t slot_offset(uint16_t s) const { return read16(HEADER_SIZE + s * SLOT_SIZE); }
    uint16_t slot_length(uint16_t s) const { return read16(HEADER_SIZE + s * SLOT_SIZE + 2); }
    void set_slot(uint16_t s, uint16_t off, uint16_t len);
    uint16_t data_start() const { return read16(10); }
    size_t live_bytes() const;
    size_t total_free(uint16_t slots) const;
    bool has_empty_slot() const;
    void write_record(uint16_t slot, std::string_view rec);

    uint8_t* d_;
};

}  // namespace atlas
