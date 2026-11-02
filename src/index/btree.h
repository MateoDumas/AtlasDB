#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace atlas {

// A B+tree node. Nodes refer to each other by id (0 = none), so a node store
// can keep them anywhere: in memory, or lazily loaded from disk pages.
template <class Key>
struct BTreeNode {
    bool leaf = true;
    std::vector<Key> keys;
    std::vector<uint32_t> children;  // inner nodes: keys.size() + 1 child ids
    uint32_t next = 0;               // leaves: id of the right sibling
    bool dirty = true;               // modified since it was last persisted
    std::vector<uint32_t> pages;     // paged stores: pages holding the serialized node
};

// Node store that simply keeps every node in memory.
template <class Key>
class MemoryNodeStore {
public:
    using Node = BTreeNode<Key>;

    Node* get(uint32_t id) const { return nodes_.at(id).get(); }
    uint32_t create(bool leaf) {
        uint32_t id = next_id_++;
        auto n = std::make_unique<Node>();
        n->leaf = leaf;
        nodes_.emplace(id, std::move(n));
        return id;
    }
    void destroy(uint32_t id) { nodes_.erase(id); }
    void clear() { nodes_.clear(); }
    bool has_meta() const { return false; }
    uint32_t meta_root() const { return 0; }
    uint64_t meta_size() const { return 0; }

private:
    std::unordered_map<uint32_t, std::unique_ptr<Node>> nodes_;
    uint32_t next_id_ = 1;
};

// B+tree storing a sorted set of unique keys.
//
// Inner nodes hold separator keys; keys[i] is a lower bound for every key in
// children[i + 1]. All keys live in the leaves, which are chained left to
// right for ordered range scans. Nodes split when they exceed `max_keys` and
// rebalance (borrow from a sibling, otherwise merge) when they drop below half.
// Every node touched by a modification is marked dirty so a persistent store
// knows what to write back.
template <class Key, class Compare = std::less<Key>, class Store = MemoryNodeStore<Key>>
class BPlusTree {
    using Node = BTreeNode<Key>;

public:
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Key;
        using difference_type = std::ptrdiff_t;
        using pointer = const Key*;
        using reference = const Key&;

        Iterator() = default;
        const Key& operator*() const { return leaf_->keys[idx_]; }
        const Key* operator->() const { return &leaf_->keys[idx_]; }
        Iterator& operator++() {
            ++idx_;
            normalize();
            return *this;
        }
        Iterator operator++(int) {
            Iterator old = *this;
            ++*this;
            return old;
        }
        bool operator==(const Iterator& o) const { return leaf_ == o.leaf_ && idx_ == o.idx_; }
        bool operator!=(const Iterator& o) const { return !(*this == o); }

    private:
        friend class BPlusTree;
        Iterator(const BPlusTree* t, const Node* l, size_t i) : tree_(t), leaf_(l), idx_(i) { normalize(); }
        void normalize() {
            while (leaf_ && idx_ >= leaf_->keys.size()) {
                leaf_ = leaf_->next ? tree_->node(leaf_->next) : nullptr;
                idx_ = 0;
            }
        }
        const BPlusTree* tree_ = nullptr;
        const Node* leaf_ = nullptr;
        size_t idx_ = 0;
    };

    // Extra arguments construct the node store in place.
    template <class... StoreArgs>
    explicit BPlusTree(size_t max_keys, StoreArgs&&... store_args)
        : max_keys_(std::max<size_t>(max_keys, 3)), store_(std::forward<StoreArgs>(store_args)...) {
        if (store_.has_meta()) {
            root_ = store_.meta_root();
            size_ = store_.meta_size();
        } else {
            root_ = store_.create(true);
        }
    }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    uint32_t root() const { return root_; }
    Store& store() { return store_; }

    void clear() {
        store_.clear();
        root_ = store_.create(true);
        size_ = 0;
    }

    // Returns false if an equal key already exists.
    bool insert(const Key& key) {
        Key sep;
        uint32_t right = 0;
        if (!insert_rec(root_, key, sep, right)) return false;
        if (right) {
            uint32_t id = store_.create(false);
            Node* r = node(id);
            r->keys.push_back(std::move(sep));
            r->children = {root_, right};
            root_ = id;
        }
        ++size_;
        return true;
    }

    // Returns false if the key was not present.
    bool erase(const Key& key) {
        if (!erase_rec(root_, key)) return false;
        Node* r = node(root_);
        if (!r->leaf && r->keys.empty()) {
            uint32_t child = r->children.front();
            store_.destroy(root_);
            root_ = child;
        }
        --size_;
        return true;
    }

    bool contains(const Key& key) const {
        auto it = lower_bound(key);
        return it != end() && !cmp_(key, *it);
    }

    Iterator begin() const {
        const Node* n = node(root_);
        while (!n->leaf) n = node(n->children.front());
        return Iterator(this, n, 0);
    }

    Iterator end() const { return Iterator(); }

    // First element not less than key.
    Iterator lower_bound(const Key& key) const {
        const Node* n = node(root_);
        while (!n->leaf) n = node(n->children[child_index(n, key)]);
        size_t i = std::lower_bound(n->keys.begin(), n->keys.end(), key, cmp_) - n->keys.begin();
        return Iterator(this, n, i);
    }

    // Height of the tree (1 = a single leaf). Exposed for tests/diagnostics.
    size_t height() const {
        size_t h = 1;
        for (const Node* n = node(root_); !n->leaf; n = node(n->children.front())) ++h;
        return h;
    }

    // Verifies structural invariants; returns false on violation (tests only).
    bool check_invariants() const {
        size_t leaf_depth = 0;
        size_t count = 0;
        if (!check_node(root_, nullptr, nullptr, 1, leaf_depth, count, true)) return false;
        return count == size_;
    }

private:
    Node* node(uint32_t id) const { return store_.get(id); }
    size_t min_keys() const { return max_keys_ / 2; }

    // Child index to descend into for key: first separator greater than key.
    size_t child_index(const Node* n, const Key& key) const {
        return std::upper_bound(n->keys.begin(), n->keys.end(), key, cmp_) - n->keys.begin();
    }

    bool insert_rec(uint32_t id, const Key& key, Key& sep_out, uint32_t& right_out) {
        Node* n = node(id);
        if (n->leaf) {
            auto it = std::lower_bound(n->keys.begin(), n->keys.end(), key, cmp_);
            if (it != n->keys.end() && !cmp_(key, *it)) return false;
            n->keys.insert(it, key);
            n->dirty = true;
            if (n->keys.size() > max_keys_) {
                uint32_t rid = store_.create(true);
                Node* r = node(rid);
                size_t mid = n->keys.size() / 2;
                r->keys.assign(std::make_move_iterator(n->keys.begin() + static_cast<std::ptrdiff_t>(mid)),
                               std::make_move_iterator(n->keys.end()));
                n->keys.erase(n->keys.begin() + static_cast<std::ptrdiff_t>(mid), n->keys.end());
                r->next = n->next;
                n->next = rid;
                sep_out = r->keys.front();
                right_out = rid;
            }
            return true;
        }

        size_t idx = child_index(n, key);
        Key child_sep;
        uint32_t child_right = 0;
        if (!insert_rec(n->children[idx], key, child_sep, child_right)) return false;
        if (child_right) {
            n->keys.insert(n->keys.begin() + static_cast<std::ptrdiff_t>(idx), std::move(child_sep));
            n->children.insert(n->children.begin() + static_cast<std::ptrdiff_t>(idx) + 1, child_right);
            n->dirty = true;
            if (n->keys.size() > max_keys_) {
                uint32_t rid = store_.create(false);
                Node* r = node(rid);
                size_t mid = n->keys.size() / 2;
                sep_out = std::move(n->keys[mid]);
                r->keys.assign(std::make_move_iterator(n->keys.begin() + static_cast<std::ptrdiff_t>(mid) + 1),
                               std::make_move_iterator(n->keys.end()));
                r->children.assign(n->children.begin() + static_cast<std::ptrdiff_t>(mid) + 1, n->children.end());
                n->keys.erase(n->keys.begin() + static_cast<std::ptrdiff_t>(mid), n->keys.end());
                n->children.erase(n->children.begin() + static_cast<std::ptrdiff_t>(mid) + 1, n->children.end());
                right_out = rid;
            }
        }
        return true;
    }

    bool erase_rec(uint32_t id, const Key& key) {
        Node* n = node(id);
        if (n->leaf) {
            auto it = std::lower_bound(n->keys.begin(), n->keys.end(), key, cmp_);
            if (it == n->keys.end() || cmp_(key, *it)) return false;
            n->keys.erase(it);
            n->dirty = true;
            return true;
        }
        size_t idx = child_index(n, key);
        if (!erase_rec(n->children[idx], key)) return false;
        if (node(n->children[idx])->keys.size() < min_keys()) rebalance(n, idx);
        return true;
    }

    void rebalance(Node* parent, size_t idx) {
        Node* nd = node(parent->children[idx]);
        Node* left = idx > 0 ? node(parent->children[idx - 1]) : nullptr;
        Node* right = idx + 1 < parent->children.size() ? node(parent->children[idx + 1]) : nullptr;
        parent->dirty = nd->dirty = true;

        if (left && left->keys.size() > min_keys()) {
            borrow_from_left(parent, idx, nd, left);
        } else if (right && right->keys.size() > min_keys()) {
            borrow_from_right(parent, idx, nd, right);
        } else if (left) {
            merge(parent, idx - 1);
        } else if (right) {
            merge(parent, idx);
        }
    }

    void borrow_from_left(Node* parent, size_t idx, Node* nd, Node* left) {
        left->dirty = true;
        if (nd->leaf) {
            nd->keys.insert(nd->keys.begin(), std::move(left->keys.back()));
            left->keys.pop_back();
            parent->keys[idx - 1] = nd->keys.front();
        } else {
            nd->keys.insert(nd->keys.begin(), std::move(parent->keys[idx - 1]));
            nd->children.insert(nd->children.begin(), left->children.back());
            parent->keys[idx - 1] = std::move(left->keys.back());
            left->keys.pop_back();
            left->children.pop_back();
        }
    }

    void borrow_from_right(Node* parent, size_t idx, Node* nd, Node* right) {
        right->dirty = true;
        if (nd->leaf) {
            nd->keys.push_back(std::move(right->keys.front()));
            right->keys.erase(right->keys.begin());
            parent->keys[idx] = right->keys.front();
        } else {
            nd->keys.push_back(std::move(parent->keys[idx]));
            nd->children.push_back(right->children.front());
            parent->keys[idx] = std::move(right->keys.front());
            right->keys.erase(right->keys.begin());
            right->children.erase(right->children.begin());
        }
    }

    // Merges children[i + 1] into children[i], removes separator i and frees the right node.
    void merge(Node* parent, size_t i) {
        uint32_t b_id = parent->children[i + 1];
        Node* a = node(parent->children[i]);
        Node* b = node(b_id);
        a->dirty = true;
        if (a->leaf) {
            a->keys.insert(a->keys.end(), std::make_move_iterator(b->keys.begin()),
                           std::make_move_iterator(b->keys.end()));
            a->next = b->next;
        } else {
            a->keys.push_back(std::move(parent->keys[i]));
            a->keys.insert(a->keys.end(), std::make_move_iterator(b->keys.begin()),
                           std::make_move_iterator(b->keys.end()));
            a->children.insert(a->children.end(), b->children.begin(), b->children.end());
        }
        parent->keys.erase(parent->keys.begin() + static_cast<std::ptrdiff_t>(i));
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        store_.destroy(b_id);
    }

    bool check_node(uint32_t id, const Key* lo, const Key* hi, size_t depth, size_t& leaf_depth, size_t& count,
                    bool is_root) const {
        const Node* n = node(id);
        if (!is_root && n->keys.size() < min_keys()) return false;
        if (n->keys.size() > max_keys_) return false;
        for (size_t i = 1; i < n->keys.size(); ++i)
            if (!cmp_(n->keys[i - 1], n->keys[i])) return false;
        for (const auto& k : n->keys) {
            if (lo && cmp_(k, *lo)) return false;
            if (hi && !cmp_(k, *hi)) return false;
        }
        if (n->leaf) {
            if (leaf_depth == 0) leaf_depth = depth;
            if (leaf_depth != depth) return false;
            count += n->keys.size();
            return true;
        }
        if (n->children.size() != n->keys.size() + 1) return false;
        for (size_t i = 0; i < n->children.size(); ++i) {
            const Key* clo = i == 0 ? lo : &n->keys[i - 1];
            const Key* chi = i == n->keys.size() ? hi : &n->keys[i];
            if (!check_node(n->children[i], clo, chi, depth + 1, leaf_depth, count, false)) return false;
        }
        return true;
    }

    size_t max_keys_;
    Compare cmp_;
    Store store_;
    uint32_t root_ = 0;
    size_t size_ = 0;
};

}  // namespace atlas
