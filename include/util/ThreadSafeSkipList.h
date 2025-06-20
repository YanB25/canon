#pragma once
#include <iostream>

namespace util::skiplist
{
template <typename K, typename V>
struct SkipListNode
{
    K k_;
    V v_;
    const K &key() const
    {
        return k_;
    }
    K &key()
    {
        return k_;
    }
    const V &value() const
    {
        return v_;
    }
    V &value()
    {
        return v_;
    }
    bool invalidate()
    {
        std::atomic<uint64_t> &atm = *(std::atomic<uint64_t> *) &deleted_;
        uint64_t expect = 0;  // false
        access_nr_ = 0;
        return atm.compare_exchange_strong(expect, 1);
    }
    bool invalid() const
    {
        return deleted_ != 0;
    }
    bool valid() const
    {
        return deleted_ == 0;
    }
    bool set_valid()
    {
        std::atomic<uint64_t> &atm = *(std::atomic<uint64_t> *) &deleted_;
        uint64_t expect = 1;  // true
        access_nr_ = 0;
        return atm.compare_exchange_strong(expect, 0);
    }
    uint64_t deleted_{0};
    mutable uint32_t access_nr_;
};
template <typename K, typename V>
inline std::ostream &operator<<(std::ostream &os, const SkipListNode<K, V> &n)
{
    os << "{" << util::pre(n.k_) << ", " << util::pre(n.v_);
    if (n.deleted_)
    {
        os << ", deleted ";
    }
    os << "(" << n.access_nr_ << ")}";
    return os;
}

template <typename K, typename V>
struct NodeComparator
{
    using DecodedType = SkipListNode<K, V>;

    static DecodedType decode_key(const char *b)
    {
        return *(DecodedType *) b;
    }

    int cmp(const DecodedType &lhs, const DecodedType &rhs) const
    {
        if (lhs.key() < rhs.key())
        {
            return -1;
        }

        if (lhs.key() > rhs.key())
        {
            return +1;
        }

        return 0;
    }

    int operator()(const char *a, const char *b) const
    {
        return cmp(decode_key(a), decode_key(b));
    }

    int operator()(const char *a, const DecodedType b) const
    {
        return cmp(decode_key(a), b);
    }
};

template <Memcpyable K, typename V>
class ThreadSafeSkipList
{
public:
    using ComparatorT = NodeComparator<K, V>;
    using SkipListT = InlineSkipList<ComparatorT>;
    using NodeT = SkipListNode<K, V>;
    ThreadSafeSkipList(ssize_t item_limit = std::numeric_limits<ssize_t>::max())
        : item_limit_(item_limit)
    {
        skiplist_ = std::make_unique<SkipListT>(cmp_, &alloc_, 21);
    }

    bool insert(const K &key, const V &val)
    {
        auto *node = (NodeT *) skiplist_->AllocateKey(sizeof(NodeT));
        node->access_nr_ = 0;
        node->k_ = key;
        node->v_ = val;
        node->deleted_ = 0;
        auto inserted = skiplist_->InsertConcurrently((const char *) node);
        if (inserted)
        {
            item_nr_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        else
        {
            // conflict
            auto *conflict_node = DCHECK_NOTNULL(find_entry(key));
            if (conflict_node->set_valid())
            {
                // I win
                conflict_node->v_ = val;
                return true;
            }
            else
            {
                // Other concurrent users win
                return false;
            }
        }
    }
    std::optional<const NodeT *> get(const K &key) const
    {
        auto *entry = find_entry(key);
        if (entry == nullptr)
        {
            LOG(INFO) << "nullptr";
            return std::nullopt;
        }
        if (!entry->valid())
        {
            LOG(INFO) << "not valid: " << *entry;
            return std::nullopt;
        }
        return DCHECK_NOTNULL(entry);
    }

    bool need_evict() const
    {
        return item_nr_.load(std::memory_order_relaxed) >= item_limit_;
    }

    std::pair<NodeT *, uint64_t> get_a_random_entry()
    {
        typename SkipListT::Iterator it(skiplist_.get());
    retry:
        auto k = fast_pseudo_rand_int(0, 100_M);
        NodeT node;
        node.k_ = k;

        it.Seek((const char *) &node);
        while (it.Valid())
        {
            auto *entry = (NodeT *) DCHECK_NOTNULL(it.key());
            if (entry->valid())
            {
                return {entry, entry->access_nr_};
            }
            // it is deleted. advance to the next entry
            it.Next();
        }
        goto retry;
    }

    std::list<NodeT *> evict_ones()
    {
        auto [e1, freq1] = get_a_random_entry();
        auto [e2, freq2] = get_a_random_entry();

        if (freq1 < freq2)
        {
            if (e1->invalidate())
            {
                item_nr_.fetch_sub(1);
                return {e1};
            }
        }
        else
        {
            if (e2->invalidate())
            {
                item_nr_.fetch_sub(1);
                return {e2};
            }
        }
        return {};
    }

private:
    std::unique_ptr<SkipListT> skiplist_;
    ssize_t item_limit_;
    mutable std::atomic<ssize_t> item_nr_{0};

    ComparatorT cmp_;
    Allocator alloc_;

    NodeT *find_entry(const K &key) const
    {
        typename SkipListT::Iterator it(skiplist_.get());
        NodeT node;
        node.k_ = key;
        it.Seek((const char *) &node);
        if (it.Valid())
        {
            auto *ret = (NodeT *) it.key();
            if (ret->k_ == key)
            {
                ret->access_nr_++;
                return ret;
            }
            else
            {
                return nullptr;
            }
        }
        else
        {
            return nullptr;
        }
    }
};

}  // namespace util::skiplist