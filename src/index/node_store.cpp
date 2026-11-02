#include "index/node_store.h"

#include <algorithm>
#include <cstring>

#include "common/bytes.h"
#include "common/error.h"

namespace atlas {

namespace {

constexpr char HEADER_MAGIC[4] = {'A', 'T', 'I', 'X'};
constexpr char FREE_MAGIC[4] = {'F', 'R', 'E', 'E'};
constexpr uint32_t STORE_VERSION = 1;
constexpr size_t CHUNK = SlottedPage::MAX_RECORD - 4;

[[noreturn]] void corrupt(const std::string& msg) { throw DbError(ErrorCode::Corruption, "index file: " + msg); }

std::string serialize(const IndexNodeStore::Node& n) {
    ByteWriter w;
    w.u8(n.leaf ? 1 : 0);
    w.u32(n.next);
    w.u32(static_cast<uint32_t>(n.keys.size()));
    for (const auto& k : n.keys) {
        w.value(k.value);
        w.u32(k.rid.page);
        w.u16(k.rid.slot);
    }
    if (!n.leaf)
        for (uint32_t c : n.children) w.u32(c);
    return w.take();
}

void deserialize(std::string_view bytes, IndexNodeStore::Node& n) {
    ByteReader r(bytes);
    n.leaf = r.u8() != 0;
    n.next = r.u32();
    uint32_t count = r.u32();
    n.keys.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        IndexKey k;
        k.value = r.value();
        k.rid.page = r.u32();
        k.rid.slot = r.u16();
        n.keys.push_back(std::move(k));
    }
    if (!n.leaf)
        for (uint32_t i = 0; i <= count; ++i) n.children.push_back(r.u32());
}

}  // namespace

IndexNodeStore::IndexNodeStore(const std::filesystem::path& path, size_t cache_pages) : pager_(path, cache_pages) {
    if (pager_.page_count() == 0) {
        pager_.allocate();  // page 0: header, written by persist()
        return;
    }
    auto h = pager_.fetch(0);
    auto rec = h.view().get(0);
    if (!rec || rec->size() < 24 || std::memcmp(rec->data(), HEADER_MAGIC, 4) != 0) corrupt("bad header");
    ByteReader r(rec->substr(4));
    if (r.u32() != STORE_VERSION) corrupt("unsupported version");
    meta_root_ = r.u32();
    meta_size_ = r.u64();
    uint32_t free_page = r.u32();
    // Walk the free list.
    while (free_page != 0) {
        if (free_page >= pager_.page_count()) corrupt("free list points past the end");
        free_pages_.push_back(free_page);
        auto fh = pager_.fetch(free_page);
        auto frec = fh.view().get(0);
        if (!frec || frec->size() != 8 || std::memcmp(frec->data(), FREE_MAGIC, 4) != 0) corrupt("bad free page");
        ByteReader fr(frec->substr(4));
        free_page = fr.u32();
        if (free_pages_.size() > pager_.page_count()) corrupt("free list cycle");
    }
}

std::unique_ptr<IndexNodeStore::Node> IndexNodeStore::load(uint32_t id) const {
    std::string bytes;
    std::vector<uint32_t> pages;
    for (uint32_t page = id; page != 0;) {
        if (page >= pager_.page_count() || pages.size() > pager_.page_count()) corrupt("bad node page chain");
        auto h = pager_.fetch(page);
        auto rec = h.view().get(0);
        if (!rec || rec->size() < 4) corrupt("missing node page " + std::to_string(page));
        ByteReader r(rec->substr(0, 4));
        pages.push_back(page);
        page = r.u32();
        bytes.append(rec->substr(4));
    }
    auto n = std::make_unique<Node>();
    deserialize(bytes, *n);
    n->pages = std::move(pages);
    n->dirty = false;
    return n;
}

IndexNodeStore::Node* IndexNodeStore::get(uint32_t id) const {
    std::lock_guard lk(mutex_);
    auto it = nodes_.find(id);
    if (it != nodes_.end()) {
        it->second->last_used = ++clock_;
        return it->second.get();
    }
    auto n = load(id);
    n->last_used = ++clock_;
    Node* raw = n.get();
    nodes_.emplace(id, std::move(n));
    loaded_ = nodes_.size();
    return raw;
}

size_t IndexNodeStore::evict(size_t capacity) {
    std::lock_guard lk(mutex_);
    if (nodes_.size() <= capacity) return 0;
    std::vector<std::pair<uint64_t, uint32_t>> clean;  // (last_used, id)
    for (const auto& [id, n] : nodes_)
        if (!n->dirty) clean.emplace_back(n->last_used, id);
    // Evict down to 3/4 of the budget so this does not run after every statement.
    size_t target = capacity - capacity / 4;
    size_t excess = nodes_.size() > target ? nodes_.size() - target : 0;
    excess = std::min(excess, clean.size());
    std::partial_sort(clean.begin(), clean.begin() + static_cast<std::ptrdiff_t>(excess), clean.end());
    for (size_t i = 0; i < excess; ++i) nodes_.erase(clean[i].second);
    loaded_ = nodes_.size();
    return excess;
}

uint32_t IndexNodeStore::allocate_page() {
    if (!free_pages_.empty()) {
        uint32_t p = free_pages_.back();
        free_pages_.pop_back();
        return p;
    }
    return pager_.allocate().id();
}

uint32_t IndexNodeStore::create(bool leaf) {
    std::lock_guard lk(mutex_);
    uint32_t id = allocate_page();
    auto n = std::make_unique<Node>();
    n->leaf = leaf;
    n->pages = {id};
    n->last_used = ++clock_;
    nodes_.emplace(id, std::move(n));
    loaded_ = nodes_.size();
    return id;
}

void IndexNodeStore::destroy(uint32_t id) {
    std::lock_guard lk(mutex_);
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return;
    for (uint32_t p : it->second->pages) free_pages_.push_back(p);
    nodes_.erase(it);
    loaded_ = nodes_.size();
}

void IndexNodeStore::clear() {
    std::lock_guard lk(mutex_);
    nodes_.clear();
    loaded_ = 0;
    free_pages_.clear();
    pager_.reset();
    pager_.allocate();  // header page
    meta_root_ = 0;
    meta_size_ = 0;
}

void IndexNodeStore::write_record(uint32_t page, const std::string& rec) {
    auto h = pager_.fetch(page);
    auto view = h.view();
    if (!view.put(0, rec)) throw DbError(ErrorCode::Internal, "index record does not fit in a page");
    h.mark_dirty();
}

void IndexNodeStore::persist(uint32_t root, uint64_t size) {
    std::lock_guard lk(mutex_);
    for (auto& [id, n] : nodes_) {
        if (!n->dirty) continue;
        std::string bytes = serialize(*n);
        size_t chunks = std::max<size_t>(1, (bytes.size() + CHUNK - 1) / CHUNK);
        while (n->pages.size() < chunks) n->pages.push_back(allocate_page());
        while (n->pages.size() > chunks) {
            free_pages_.push_back(n->pages.back());
            n->pages.pop_back();
        }
        for (size_t i = 0; i < chunks; ++i) {
            ByteWriter w;
            w.u32(i + 1 < chunks ? n->pages[i + 1] : 0);
            size_t off = i * CHUNK;
            w.raw(bytes.data() + off, std::min(CHUNK, bytes.size() - off));
            write_record(n->pages[i], w.data());
        }
        n->dirty = false;
    }
    // Chain the free pages together so they can be reused after a restart.
    for (size_t i = 0; i < free_pages_.size(); ++i) {
        ByteWriter w;
        w.raw(FREE_MAGIC, 4);
        w.u32(i + 1 < free_pages_.size() ? free_pages_[i + 1] : 0);
        write_record(free_pages_[i], w.data());
    }
    ByteWriter h;
    h.raw(HEADER_MAGIC, 4);
    h.u32(STORE_VERSION);
    h.u32(root);
    h.u64(size);
    h.u32(free_pages_.empty() ? 0 : free_pages_.front());
    write_record(0, h.data());
    meta_root_ = root;
    meta_size_ = size;
}

}  // namespace atlas
