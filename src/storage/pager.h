#pragma once

#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "storage/file.h"
#include "storage/page.h"

namespace atlas {

struct Page {
    uint32_t id = 0;
    std::array<uint8_t, PAGE_SIZE> data{};
    bool dirty = false;
    int pins = 0;
    std::list<uint32_t>::iterator lru_pos;
};

class Pager;

// RAII pin on a cached page. While a handle is alive the page cannot be evicted.
class PageHandle {
public:
    PageHandle() = default;
    PageHandle(Pager* pager, Page* page);
    ~PageHandle();
    PageHandle(const PageHandle&) = delete;
    PageHandle& operator=(const PageHandle&) = delete;
    PageHandle(PageHandle&& o) noexcept : pager_(o.pager_), page_(o.page_) { o.page_ = nullptr; }
    PageHandle& operator=(PageHandle&& o) noexcept;

    uint32_t id() const { return page_->id; }
    SlottedPage view() { return SlottedPage(page_->data.data()); }
    void mark_dirty() { page_->dirty = true; }

private:
    void release();
    Pager* pager_ = nullptr;
    Page* page_ = nullptr;
};

// Buffer pool for a single file of fixed-size pages.
//
// Policy is "no-steal": dirty pages are never written back by eviction, only by
// flush() (called at checkpoints, when no transaction is active). Uncommitted
// data therefore never reaches the data files, which keeps recovery redo-only.
//
// The cache bookkeeping (map, LRU list, pin counts) is guarded by a mutex so
// that several readers holding the shared transaction lock can fetch pages
// concurrently. Page contents are only modified under the exclusive lock.
class Pager {
public:
    Pager(std::filesystem::path path, size_t capacity);

    uint32_t page_count() const {
        std::lock_guard lk(mutex_);
        return page_count_;
    }

    PageHandle fetch(uint32_t id);
    PageHandle allocate();
    // Like fetch, but creates (empty) pages up to `id` if the file is shorter. Used by recovery.
    PageHandle fetch_or_create(uint32_t id);

    // Copies of all dirty pages (with fresh checksums), for the doublewrite buffer.
    std::vector<std::pair<uint32_t, std::array<uint8_t, PAGE_SIZE>>> dirty_copies();
    // Writes all dirty pages and fsyncs the file.
    void flush();
    // Drops all pages and truncates the file to zero length.
    void reset();
    size_t cached_pages() const {
        std::lock_guard lk(mutex_);
        return cache_.size();
    }
    size_t dirty_pages() const;

private:
    friend class PageHandle;
    void unpin(Page* p);
    Page* load(uint32_t id);
    void evict_if_needed();
    Page* allocate_locked();

    mutable std::mutex mutex_;
    File file_;
    size_t capacity_;
    uint32_t page_count_ = 0;
    std::unordered_map<uint32_t, std::unique_ptr<Page>> cache_;
    std::list<uint32_t> lru_;  // front = most recently used
};

}  // namespace atlas
