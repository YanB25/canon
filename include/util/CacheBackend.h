#pragma once
#include <mutex>
#include <utility>

#include "CoroContext.h"
#include "util/History.h"

namespace util
{
template <typename K, typename V>
class ICacheBackend
{
public:
    using Entry = std::pair<K, V>;
    using pointer = std::shared_ptr<ICacheBackend>;

    virtual V get_async(const K &, CoroContext *) = 0;
    virtual void put_async(const K &, const V &, CoroContext *) = 0;
    virtual V get(const K &k, CoroContext *ctx)
    {
        auto ret = get_async(k, ctx);
        sync(ctx);
        return ret;
    }
    virtual void put(const K &k, const V &v, CoroContext *ctx)
    {
        put_async(k, v, ctx);
        sync(ctx);
    }

    // wait until all the async command has finished
    virtual void sync(CoroContext *ctx) = 0;
    virtual ~ICacheBackend() = default;

private:
};

template <typename K, typename V>
class EmptyBackend : public ICacheBackend<K, V>
{
public:
    using Base = ICacheBackend<K, V>;
    using Entry = typename Base::Entry;
    static std::shared_ptr<EmptyBackend> make_ptr()
    {
        return std::make_shared<EmptyBackend>();
    }
    void put_async(const K &, const V &, CoroContext *) override
    {
    }
    V get_async(const K &, CoroContext *) override
    {
        return V{};
    }

    // wait until all the async command has finished
    void sync(CoroContext *) override
    {
    }
    size_t miss_nr() const
    {
        auto f = [](uint64_t acc, uint64_t cur) { return acc + cur; };
        return miss_nr_.accumulate(f, 0ull);
    }

private:
    Perthread<uint64_t> miss_nr_;
};

template <typename K, typename V>
class CounterBackend : public ICacheBackend<K, V>
{
public:
    using Base = ICacheBackend<K, V>;
    using Entry = typename Base::Entry;
    static std::shared_ptr<CounterBackend> make_ptr()
    {
        return std::make_shared<CounterBackend>();
    }
    void put_async(const K &, const V &, CoroContext *) override
    {
        evicted_.current()++;
    }
    V get_async(const K &, CoroContext *) override
    {
        LOG(FATAL) << "** Not backend provided";
        return V{};
    }

    // wait until all the async command has finished
    void sync(CoroContext *) override
    {
    }
    size_t evicted_nr() const
    {
        auto f = [](const size_t &acc, const size_t &cur) -> size_t
        { return acc + cur; };
        return evicted_.accumulate<size_t>(f);
    }

private:
    Perthread<size_t> evicted_;
};

template <typename K, typename V>
class MapBackend : public ICacheBackend<K, V>
{
public:
    using Base = ICacheBackend<K, V>;
    using Entry = typename Base::Entry;
    static std::shared_ptr<MapBackend> make_ptr()
    {
        return std::make_shared<MapBackend>();
    }
    void put_async(const K &k, const V &v, CoroContext *) override
    {
        std::lock_guard lk(mu_);
        history_.add(std::make_pair(k, v));
        map_[k] = v;
    }
    V get_async(const K &k, CoroContext *) override
    {
        std::lock_guard lk(mu_);
        auto it = map_.find(k);
        if (it == map_.end())
        {
            LOG(FATAL) << "Unexpected miss: " << PRE(k);
        }
        return it->second;
    }

    // wait until all the async command has finished
    void sync(CoroContext *) override
    {
    }
    size_t evicted_nr() const
    {
        std::lock_guard lk(mu_);
        return map_.size();
    }

    std::map<K, V> &map()
    {
        return map_;
    }

    // for debug purpose: return the operation history
    const auto &history() const
    {
        return history_;
    }

private:
    std::map<K, V> map_;
    util::History<std::pair<K, V>> history_;
    mutable std::mutex mu_;
};

}  // namespace util