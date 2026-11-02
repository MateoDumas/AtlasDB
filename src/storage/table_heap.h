#pragma once

#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "storage/pager.h"

namespace atlas {

// Physical row identifier: page number + slot inside the page.
struct RID {
    uint32_t page = 0;
    uint16_t slot = 0;
    auto operator<=>(const RID&) const = default;
    std::string to_string() const { return "(" + std::to_string(page) + "," + std::to_string(slot) + ")"; }
};

// Unordered collection of records stored in slotted pages of one file.
class TableHeap {
public:
    TableHeap(std::filesystem::path path, size_t cache_pages);

    // Inserts anywhere with room and returns the new location.
    RID insert(std::string_view rec);
    // Places a record at an exact location (undo / redo).
    void put(RID rid, std::string_view rec);
    // Tries to overwrite in place; returns false if the new record does not fit on the page.
    bool update(RID rid, std::string_view rec);
    void erase(RID rid);
    std::optional<std::string> get(RID rid);

    void set_page_lsn(uint32_t page, uint64_t lsn);
    uint64_t page_lsn(uint32_t page);
    // Recovery: ensure the page exists and return its LSN.
    uint64_t page_lsn_for_redo(uint32_t page);

    uint32_t page_count() const { return pager_.page_count(); }
    // Advances `rid` to the next live record at or after `from`. Returns false at the end.
    bool next(RID from, RID& rid, std::string& rec);

    void rebuild_free_space_map();
    void flush() { pager_.flush(); }
    void reset();
    Pager& pager() { return pager_; }

private:
    void note_free(uint32_t page, size_t free);

    // Free bytes per page; UNKNOWN until the page is first looked at, so
    // opening a table does not have to read all of it.
    static constexpr uint16_t UNKNOWN = 0xFFFF;
    Pager pager_;
    std::vector<uint16_t> free_;
    uint32_t hint_ = 0;
};

}  // namespace atlas
