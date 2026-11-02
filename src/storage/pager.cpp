#include "storage/pager.h"

#include <algorithm>
#include <vector>

#include "common/error.h"

namespace atlas {

PageHandle::PageHandle(Pager* pager, Page* page) : pager_(pager), page_(page) { ++page_->pins; }

PageHandle::~PageHandle() { release(); }

PageHandle& PageHandle::operator=(PageHandle&& o) noexcept {
    if (this != &o) {
        release();
        pager_ = o.pager_;
        page_ = o.page_;
        o.page_ = nullptr;
    }
    return *this;
}

void PageHandle::release() {
    if (page_) {
        pager_->unpin(page_);
        page_ = nullptr;
    }
}

Pager::Pager(std::filesystem::path path, size_t capacity)
    : file_(File::open(path)), capacity_(std::max<size_t>(capacity, 8)) {
    page_count_ = static_cast<uint32_t>(file_.size() / PAGE_SIZE);
}

void Pager::unpin(Page* p) {
    std::lock_guard lk(mutex_);
    --p->pins;
}

size_t Pager::dirty_pages() const {
    std::lock_guard lk(mutex_);
    size_t n = 0;
    for (const auto& [id, p] : cache_) n += p->dirty ? 1 : 0;
    return n;
}

void Pager::evict_if_needed() {
    // Walk from least recently used; skip pinned or dirty pages (no-steal).
    auto it = lru_.end();
    while (cache_.size() >= capacity_ && it != lru_.begin()) {
        --it;
        auto found = cache_.find(*it);
        Page* p = found->second.get();
        if (p->pins == 0 && !p->dirty) {
            it = lru_.erase(it);
            cache_.erase(found);
        }
    }
}

Page* Pager::load(uint32_t id) {
    if (auto it = cache_.find(id); it != cache_.end()) {
        Page* p = it->second.get();
        lru_.splice(lru_.begin(), lru_, p->lru_pos);
        return p;
    }
    evict_if_needed();
    auto page = std::make_unique<Page>();
    page->id = id;
    size_t n = file_.read_at(static_cast<uint64_t>(id) * PAGE_SIZE, page->data.data(), PAGE_SIZE);
    SlottedPage view(page->data.data());
    if (n < PAGE_SIZE || view.is_zeroed()) {
        // Beyond EOF or a hole left by an interrupted checkpoint: treat as empty.
        view.init();
    } else if (!view.is_initialized()) {
        throw DbError(ErrorCode::Corruption,
                      "page " + std::to_string(id) + " of '" + file_.path().string() + "' is corrupt");
    } else if (!view.verify_checksum()) {
        throw DbError(ErrorCode::Corruption, "checksum mismatch on page " + std::to_string(id) + " of '" +
                                                 file_.path().string() + "' (torn write or disk corruption)");
    }
    lru_.push_front(id);
    page->lru_pos = lru_.begin();
    Page* raw = page.get();
    cache_.emplace(id, std::move(page));
    return raw;
}

PageHandle Pager::fetch(uint32_t id) {
    std::lock_guard lk(mutex_);
    if (id >= page_count_) throw DbError(ErrorCode::Internal, "page " + std::to_string(id) + " out of range");
    return PageHandle(this, load(id));
}

Page* Pager::allocate_locked() {
    uint32_t id = page_count_++;
    Page* p = load(id);
    SlottedPage(p->data.data()).init();
    p->dirty = true;
    return p;
}

PageHandle Pager::allocate() {
    std::lock_guard lk(mutex_);
    return PageHandle(this, allocate_locked());
}

PageHandle Pager::fetch_or_create(uint32_t id) {
    std::lock_guard lk(mutex_);
    while (page_count_ <= id) allocate_locked();
    return PageHandle(this, load(id));
}

std::vector<std::pair<uint32_t, std::array<uint8_t, PAGE_SIZE>>> Pager::dirty_copies() {
    std::lock_guard lk(mutex_);
    std::vector<std::pair<uint32_t, std::array<uint8_t, PAGE_SIZE>>> out;
    for (auto& [id, p] : cache_) {
        if (!p->dirty) continue;
        SlottedPage(p->data.data()).update_checksum();
        out.emplace_back(id, p->data);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

void Pager::flush() {
    std::lock_guard lk(mutex_);
    std::vector<Page*> dirty;
    for (auto& [id, p] : cache_)
        if (p->dirty) dirty.push_back(p.get());
    std::sort(dirty.begin(), dirty.end(), [](Page* a, Page* b) { return a->id < b->id; });
    for (Page* p : dirty) {
        SlottedPage(p->data.data()).update_checksum();
        file_.write_at(static_cast<uint64_t>(p->id) * PAGE_SIZE, p->data.data(), PAGE_SIZE);
        p->dirty = false;
    }
    if (!dirty.empty()) file_.sync();
    evict_if_needed();
}

void Pager::reset() {
    std::lock_guard lk(mutex_);
    cache_.clear();
    lru_.clear();
    page_count_ = 0;
    file_.truncate(0);
}

}  // namespace atlas
