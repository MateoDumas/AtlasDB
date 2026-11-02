#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/value.h"
#include "index/btree.h"
#include "storage/pager.h"
#include "storage/table_heap.h"

namespace atlas {

// Index entries are (value, rid) pairs, so duplicates of a value are distinct
// keys and a plain set-like B+tree suffices for non-unique indexes too.
struct IndexKey {
    Value value;
    RID rid;
};

struct IndexKeyLess {
    bool operator()(const IndexKey& a, const IndexKey& b) const {
        int c = compare_values(a.value, b.value);
        if (c != 0) return c < 0;
        return a.rid < b.rid;
    }
};

// B+tree node store backed by an index file.
//
// File layout (every page is a slotted page holding a single record):
//   page 0      header: "ATIX", version, root node id, entry count, free-list head
//   pages 1..   node chunks: [u32 next page of this node, 0 = last][serialized node bytes]
//               free pages:  ["FREE"][u32 next free page]
// A node's id is the number of its first page. Large nodes (long TEXT keys)
// simply span several chained pages.
//
// Nodes are loaded lazily on first access and kept in memory; persist() writes
// the dirty ones back into pages, which reach the disk at the next checkpoint.
// get() is safe to call from several readers at once.
class IndexNodeStore {
public:
    using Node = BTreeNode<IndexKey>;

    IndexNodeStore(const std::filesystem::path& path, size_t cache_pages);

    Node* get(uint32_t id) const;
    uint32_t create(bool leaf);
    void destroy(uint32_t id);
    void clear();

    bool has_meta() const { return meta_root_ != 0; }
    uint32_t meta_root() const { return meta_root_; }
    uint64_t meta_size() const { return meta_size_; }

    // Serializes dirty nodes, the free list and the header into pages.
    void persist(uint32_t root, uint64_t size);
    Pager& pager() { return pager_; }
    size_t loaded_nodes() const;

private:
    uint32_t allocate_page();
    void write_record(uint32_t page, const std::string& rec);
    std::unique_ptr<Node> load(uint32_t id) const;

    mutable std::mutex mutex_;
    mutable Pager pager_;
    mutable std::unordered_map<uint32_t, std::unique_ptr<Node>> nodes_;
    std::vector<uint32_t> free_pages_;
    uint32_t meta_root_ = 0;
    uint64_t meta_size_ = 0;
};

}  // namespace atlas
