#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "catalog/schema.h"
#include "common/value.h"
#include "index/btree.h"
#include "index/node_store.h"
#include "storage/table_heap.h"

namespace atlas {

struct IndexBound {
    Value value;
    bool inclusive = true;
};

// Secondary index over a single column of a table, stored in its own file
// (see IndexNodeStore). Node changes stay in memory until persist(), which the
// database calls at every checkpoint.
class Index {
public:
    static constexpr size_t MAX_KEYS = 128;

    Index(IndexDef def, int column, const std::filesystem::path& file, size_t cache_pages)
        : def_(std::move(def)), column_(column), tree_(MAX_KEYS, file, cache_pages) {}

    const IndexDef& def() const { return def_; }
    int column() const { return column_; }
    size_t size() const { return tree_.size(); }

    void insert(const Value& v, RID rid) { tree_.insert(IndexKey{v, rid}); }
    void erase(const Value& v, RID rid) { tree_.erase(IndexKey{v, rid}); }
    void clear() { tree_.clear(); }

    // Writes modified nodes into the index file's pages (not yet to disk).
    void persist() { tree_.store().persist(tree_.root(), tree_.size()); }
    Pager& pager() { return tree_.store().pager(); }
    size_t loaded_nodes() { return tree_.store().loaded_nodes(); }

    // Returns the rid of some entry with value v other than `self`, if any.
    std::optional<RID> find_other(const Value& v, std::optional<RID> self = std::nullopt) const {
        for (auto it = tree_.lower_bound(IndexKey{v, RID{0, 0}}); it != tree_.end(); ++it) {
            if (compare_values(it->value, v) != 0) break;
            if (!self || it->rid != *self) return it->rid;
        }
        return std::nullopt;
    }

    std::vector<RID> lookup(const Value& v) const {
        std::vector<RID> out;
        if (v.is_null()) return out;
        for (auto it = tree_.lower_bound(IndexKey{v, RID{0, 0}}); it != tree_.end(); ++it) {
            if (compare_values(it->value, v) != 0) break;
            out.push_back(it->rid);
        }
        return out;
    }

    // Rids with lo <op> value <op> hi in key order; NULL values are never returned.
    std::vector<RID> range(const std::optional<IndexBound>& lo, const std::optional<IndexBound>& hi) const {
        std::vector<RID> out;
        auto it = lo ? tree_.lower_bound(IndexKey{lo->value, RID{0, 0}}) : tree_.begin();
        for (; it != tree_.end(); ++it) {
            const Value& v = it->value;
            if (v.is_null()) continue;
            if (lo && !lo->inclusive && compare_values(v, lo->value) == 0) continue;
            if (hi) {
                int c = compare_values(v, hi->value);
                if (c > 0 || (c == 0 && !hi->inclusive)) break;
            }
            out.push_back(it->rid);
        }
        return out;
    }

    bool check_invariants() const { return tree_.check_invariants(); }

private:
    IndexDef def_;
    int column_;
    BPlusTree<IndexKey, IndexKeyLess, IndexNodeStore> tree_;
};

}  // namespace atlas
