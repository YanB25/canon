// https://github.com/lamerman/cpp-lru-cache
#pragma once
#ifndef _LRUCACHE_HPP_INCLUDED_
#define _LRUCACHE_HPP_INCLUDED_

#include <cstddef>
#include <list>
#include <stdexcept>
#include <unordered_map>

#include "glog/logging.h"
#include "util/Likely.h"
#include "util/Pre.h"

namespace util
{
// NOT THREAD SAFE
template <typename K, typename V>
class LRUCache
{
public:
    typedef typename std::pair<K, V> Entry;
    typedef typename std::list<Entry>::iterator list_iterator_t;

    LRUCache(size_t max_size) : max_size_(std::max((size_t) 1, max_size))
    {
    }

    /**
     * @brief put a key, value into the LRU cache
     * list and map are manipulated accordingly
     *
     * @return
     * - std::nullopt: inserted a new entry
     * - optional<V>: the overwrite value
     */
    template <typename V2>
    std::optional<V> put(const K &key, V2 &&value, bool is_insert)
    {
        std::optional<V> opt;

        auto it = map_.find(key);
        if (it != map_.end())
        {
            // key exists
            auto &list_it = it->second;
            if (!is_insert)
            {
                opt.emplace(std::move(list_it->second));
                list_it->second = std::forward<V2>(value);
            }
            list_.splice(list_.begin(), list_, list_it);
            // no need to touch map
            // because list.splice does not invalidate list_it
        }
        else
        {
            // key does not exist
            list_.emplace_front(key, std::forward<V2>(value));
            DCHECK_EQ(map_.count(key), 0);
            map_[key] = list_.begin();
        }
        return opt;
    }

    bool empty() const
    {
        DCHECK_EQ(list_.size(), map_.size());
        return list_.empty();
    }

    // evict one, unless empty()
    std::optional<Entry> force_evict()
    {
        std::optional<Entry> ret;
        if (unlikely(empty()))
        {
            return ret;
        }
        ret.emplace(std::move(list_.back()));

        auto &entry = ret.value();
        map_.erase(entry.first);
        list_.pop_back();

        return ret;
    }

    // evict one, unless !need_evict()
    std::optional<Entry> evict()
    {
        std::optional<Entry> ret;
        if (likely(need_evict()))
        {
            return force_evict();
        }
        return ret;
    }
    std::optional<V> invalidate(const K &key)
    {
        std::optional<V> ret;
        auto it = map_.find(key);
        if (it != map_.end())
        {
            // hit
            auto &list_it = it->second;
            ret.emplace(std::move(list_it->second));
            list_.erase(list_it);
            map_.erase(it);
        }
        else
        {
            // miss
        }
        return ret;
    }

    // the @get API has to return a copy,
    // because an LRU entry can be evicted at any time.
    std::optional<V> get(const K &key)
    {
        auto it = map_.find(key);
        if (it == map_.end())
        {
            return std::nullopt;
        }
        else
        {
            list_.splice(list_.begin(), list_, it->second);
            return it->second->second;
        }
    }
    std::optional<V> get_nolru(const K &key) const
    {
        auto it = map_.find(key);
        if (it == map_.end())
        {
            return std::nullopt;
        }
        else
        {
            return it->second->second;
        }
    }
    // provide the @exist API in case the value is too large: save one copy
    bool exist_nolru(const K &key) const
    {
        return map_.find(key) != map_.end();
    }

    size_t size() const
    {
        return map_.size();
    }
    template <typename K2, typename V2>
    friend std::ostream &operator<<(std::ostream &os,
                                    const util::pre<LRUCache<K2, V2>> &c);

    std::pair<std::list<Entry> &, std::unordered_map<K, list_iterator_t> &>
    inner()
    {
        return {list_, map_};
    }

    bool need_evict() const
    {
        return map_.size() > max_size_;
    }

    void clear()
    {
        list_.clear();
        map_.clear();
    }

private:
    std::list<Entry> list_;
    std::unordered_map<K, list_iterator_t> map_;
    size_t max_size_;
};

template <typename K, typename V>
inline std::ostream &operator<<(std::ostream &os,
                                const util::pre<LRUCache<K, V>> &c)
{
    const auto &t = c.inner();
    os << "{LRUCache ";
    if (c.limit())
    {
        os << "items: " << util::pre(t.list_, 10);
    }
    os << "}";
    return os;
}

}  // namespace util

#endif /* _LRUCACHE_HPP_INCLUDED_ */
