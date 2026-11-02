#include "storage/table_heap.h"

#include "common/error.h"

namespace atlas {

TableHeap::TableHeap(std::filesystem::path path, size_t cache_pages) : pager_(std::move(path), cache_pages) {
    rebuild_free_space_map();
}

void TableHeap::note_free(uint32_t page, size_t free) {
    if (free_.size() <= page) free_.resize(page + 1, UNKNOWN);
    free_[page] = static_cast<uint16_t>(free);
}

void TableHeap::rebuild_free_space_map() {
    free_.assign(pager_.page_count(), UNKNOWN);
    // Start inserting at the last page: it is the one most likely to have room.
    hint_ = pager_.page_count() ? pager_.page_count() - 1 : 0;
}

RID TableHeap::insert(std::string_view rec) {
    if (rec.size() > SlottedPage::MAX_RECORD)
        throw DbError(ErrorCode::Constraint, "row too large (" + std::to_string(rec.size()) + " bytes, max " +
                                                 std::to_string(SlottedPage::MAX_RECORD) + ")");
    uint32_t n = pager_.page_count();
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t p = (hint_ + i) % n;
        if (p < free_.size() && free_[p] != UNKNOWN && free_[p] < rec.size()) continue;
        auto h = pager_.fetch(p);
        auto view = h.view();
        if (auto slot = view.insert(rec)) {
            h.mark_dirty();
            note_free(p, view.free_space());
            hint_ = p;
            return RID{p, *slot};
        }
        note_free(p, view.free_space());
    }
    auto h = pager_.allocate();
    auto view = h.view();
    auto slot = view.insert(rec);
    if (!slot) throw DbError(ErrorCode::Internal, "record does not fit in an empty page");
    note_free(h.id(), view.free_space());
    hint_ = h.id();
    return RID{h.id(), *slot};
}

void TableHeap::put(RID rid, std::string_view rec) {
    auto h = pager_.fetch_or_create(rid.page);
    auto view = h.view();
    if (!view.put(rid.slot, rec))
        throw DbError(ErrorCode::Internal, "record does not fit at " + rid.to_string());
    h.mark_dirty();
    note_free(rid.page, view.free_space());
}

bool TableHeap::update(RID rid, std::string_view rec) {
    auto h = pager_.fetch(rid.page);
    auto view = h.view();
    if (!view.update(rid.slot, rec)) return false;
    h.mark_dirty();
    note_free(rid.page, view.free_space());
    return true;
}

void TableHeap::erase(RID rid) {
    if (rid.page >= pager_.page_count()) return;
    auto h = pager_.fetch(rid.page);
    auto view = h.view();
    view.erase(rid.slot);
    h.mark_dirty();
    note_free(rid.page, view.free_space());
}

std::optional<std::string> TableHeap::get(RID rid) {
    if (rid.page >= pager_.page_count()) return std::nullopt;
    auto h = pager_.fetch(rid.page);
    auto rec = h.view().get(rid.slot);
    if (!rec) return std::nullopt;
    return std::string(*rec);
}

void TableHeap::set_page_lsn(uint32_t page, uint64_t lsn) {
    auto h = pager_.fetch(page);
    h.view().set_lsn(lsn);
    h.mark_dirty();
}

uint64_t TableHeap::page_lsn(uint32_t page) {
    auto h = pager_.fetch(page);
    return h.view().lsn();
}

uint64_t TableHeap::page_lsn_for_redo(uint32_t page) {
    auto h = pager_.fetch_or_create(page);
    return h.view().lsn();
}

bool TableHeap::next(RID from, RID& rid, std::string& rec) {
    for (uint32_t p = from.page; p < pager_.page_count(); ++p) {
        auto h = pager_.fetch(p);
        auto view = h.view();
        uint16_t start = p == from.page ? from.slot : 0;
        for (uint16_t s = start; s < view.slot_count(); ++s) {
            if (auto r = view.get(s)) {
                rid = RID{p, s};
                rec.assign(r->data(), r->size());
                return true;
            }
        }
    }
    return false;
}

void TableHeap::reset() {
    pager_.reset();
    free_.clear();
    hint_ = 0;
}

}  // namespace atlas
