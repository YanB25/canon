#pragma once
#include <city.h>

#include <array>
#include <atomic>
#include <cinttypes>
#include <list>
#include <optional>

#include "PerThread.h"
#include "thirdparty/skiplist/inlineskiplist.h"
#include "util/CacheBackend.h"
#include "util/History.h"
#include "util/LRU.h"
#include "util/Rand.h"
#include "util/Type.h"
#include "util/lock/Guard.h"
#include "util/lock/RWLock.h"

namespace util::hash
{
template <typename K, typename V>
using Entry = std::pair<K, V>;

template <typename K, typename V>
struct Node
{
    using EntryT = Entry<K, V>;
    template <typename T1, typename T2>
    Node(T1 &&k, T2 &&v) : entry_(std::forward<T1>(k), std::forward<T2>(v))
    {
    }

    const EntryT &entry() const
    {
        return entry_;
    }
    EntryT &entry()
    {
        return entry_;
    }

    const K &key() const
    {
        return entry_.value().first;
    }
    K &key()
    {
        return entry_.first;
    }
    const V &value() const
    {
        return entry_.second;
    }
    V &value()
    {
        return entry_.second;
    }

    EntryT entry_;
};

enum class TagState
{
    kFetching,
    kEvicting,
};
inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] ::util::hash::TagState e)
{
    switch (e)
    {
    case ::util::hash::TagState::kFetching:
        os << "kFetching";
        break;
    case ::util::hash::TagState::kEvicting:
        os << "kEvicting";
        break;
    default:
        os << "Unknown ::util::hash::TagState(" << (int) e << ")";
    }
    return os;
}

struct Tag
{
    Tag(TagState state, uint64_t id) : id_(id), state_(state)
    {
    }
    void join(void *wait_variable)
    {
        wait_set_.insert((std::atomic<bool> *) (wait_variable));
    }
    TagState state() const
    {
        return state_;
    }
    size_t wait_size() const
    {
        return wait_set_.size();
    }
    uint64_t id() const
    {
        return id_.val;
    }
    std::pair<uint32_t, uint32_t> id_detail() const
    {
        return from_id(id());
    }
    const std::set<std::atomic<bool> *> &wait_set() const
    {
        return wait_set_;
    }
    static uint64_t to_id(uint32_t thread_id, uint32_t coro_id)
    {
        compound_uint64_t ret(thread_id, coro_id);
        return ret.val;
    }
    static std::pair<uint32_t, uint32_t> from_id(uint64_t id)
    {
        compound_uint64_t from(id);
        return {from.u32_1, from.u32_2};
    }

    // id: who adds this tag
    // u32_1: thread_id, u32_2: coro_id
    compound_uint64_t id_;
    TagState state_;
    // pointers to the "ready" boolean variable
    std::set<std::atomic<bool> *> wait_set_;
};

enum Action
{
    kPut,
    kGet,
    kInvalidate,
    kEvictOnPut,
    kEvictDone,
    kEvictOnGet,
    kFetchOnGet,
    kLRUPut,
    kLRUGet,
    kLRUEvict,
    kLRUInvalidate,
    kLRUPutOnGet,
    kTagWait,
};
inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] ::util::hash::Action e)
{
    switch (e)
    {
    case ::util::hash::kPut:
        os << "kPut";
        break;
    case ::util::hash::kGet:
        os << "kGet";
        break;
    case ::util::hash::kInvalidate:
        os << "kInvalidate";
        break;
    case ::util::hash::kEvictOnPut:
        os << "kEvictOnPut";
        break;
    case ::util::hash::kEvictDone:
        os << "kEvictDone";
        break;
    case ::util::hash::kEvictOnGet:
        os << "kEvictOnGet";
        break;
    case ::util::hash::kFetchOnGet:
        os << "kFetchOnGet";
        break;
    case ::util::hash::kLRUPut:
        os << "kLRUPut";
        break;
    case ::util::hash::kLRUGet:
        os << "kLRUGet";
        break;
    case ::util::hash::kLRUEvict:
        os << "kLRUEvict";
        break;
    case ::util::hash::kLRUInvalidate:
        os << "kLRUInvalidate";
        break;
    case ::util::hash::kLRUPutOnGet:
        os << "kLRUPutOnGet";
        break;
    case ::util::hash::kTagWait:
        os << "kTagWait";
        break;
    default:
        os << "Unknown ::util::hash::Action(" << (int) e << ")";
    }
    return os;
}

template <typename K, typename V>
struct ActionRecord
{
    K key;
    V value;
    Action act;
    std::optional<V> value_2;
    std::list<uint32_t> wait_list;
};

struct TagRecord
{
    const char *act;
    void *cv{nullptr};
    uint32_t tid;
    uint32_t cid;
};

template <typename K, typename V>
inline std::ostream &operator<<(
    std::ostream &os,
    [[maybe_unused]] const ::util::hash::ActionRecord<K, V> &v)
{
    os << "{ ";
    os << "key: " << util::pre(v.key);
    os << ", value: " << util::pre(v.value);
    os << ", act: " << util::pre(v.act);
    if (!v.wait_list.empty())
    {
        os << ", wait: " << util::pre(v.wait_list);
    }
    os << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::util::hash::Tag &v)
{
    auto [tid, coro_id] = v.id_detail();
    os << "{Tag ";
    os << ", state_: " << util::pre(v.state_);
    os << ", tid: " << util::pre(tid);
    os << ", coro_id: " << util::pre(coro_id);
    os << "}";
    return os;
}

template <typename K, typename V>
inline std::ostream &operator<<(std::ostream &os, const Node<K, V> &n)
{
    os << "{" << util::pre(n.entry()) << "}";
    return os;
}

struct CacheMetric
{
    size_t read_nr{0};
    size_t read_miss{0};
    size_t read_evict{0};
    size_t write_nr{0};
    size_t write_evict{0};
    size_t invalidate_nr{0};
    size_t invalidate_put{0};
    constexpr size_t total_op() const
    {
        return read_nr + write_nr + invalidate_nr;
    }
    constexpr size_t total_io() const
    {
        return read_miss + read_evict + write_evict + invalidate_put;
    }
    constexpr double io_rate() const
    {
        return 1.0 * total_io() / total_op();
    }
    CacheMetric &operator+=(const CacheMetric &rhs)
    {
        read_nr += rhs.read_nr;
        read_miss += rhs.read_miss;
        read_evict += rhs.read_evict;
        write_nr += rhs.write_nr;
        write_evict += rhs.write_evict;
        invalidate_nr += rhs.invalidate_nr;
        invalidate_put += rhs.invalidate_put;
        return *this;
    }
    CacheMetric operator+(const CacheMetric &rhs) const
    {
        CacheMetric ret = *this;
        ret += rhs;
        return ret;
    }
};

template <typename K, typename V>
class ThreadSafeHashCache
{
public:
    using InnerV = std::shared_ptr<V>;
    using TagListT = std::multimap<K, Tag>;
    using EntryT = typename Node<K, InnerV>::EntryT;
    using LRU = util::LRUCache<K, InnerV>;
    using ICacheBackendPtr = typename ICacheBackend<K, V>::pointer;
    using ICacheBackendT = ICacheBackend<K, V>;
    using ActionRecordT = ActionRecord<K, V>;
    constexpr static bool kDebug = false;
    ThreadSafeHashCache(ICacheBackendPtr backend,
                        size_t bucket_nr,
                        ssize_t item_limit)

        : bucket_nr_(bucket_nr),
          item_limit_(item_limit),
          lrus_(bucket_nr, item_limit / bucket_nr),
          backend_(backend),
          tags_(bucket_nr),
          rw_locks_(bucket_nr)
    {
        item_nr_.fill(0);
        expect_bucket_limit_ = std::max((size_t) 1, item_limit / bucket_nr);
    }
    // invalidate an item
    // return
    // - std::nullopt: key does not exist
    // - V: the invalidated value
    std::shared_ptr<V> invalidate(const K &key, CoroContext *ctx = nullptr)
    {
        metric_.current().invalidate_nr++;

        auto [lru, tag_list, lk] = locate(key);
        auto tid = util::get_thread_id();
        compound_uint64_t id(tid, ctx ? ctx->coro_id() : kNotACoro);

        // TODO: make this function trigger eviction too
        std::shared_ptr<V> ret;

        wait_until_taglist_clean_locked(tag_list, lk, key, ctx);
        {
            // when reach here, lk is LOCKED and tag_list is clean with key

            ret = bucket_invalidate(lru, key).value_or(nullptr);
            if (ret)
            {
                debug_record_action(key, *ret.get(), Action::kLRUInvalidate);
                auto *cv = taglist_add(
                    tag_list, key, TagState::kEvicting, id.val, ctx);
                DCHECK_EQ(cv, nullptr)
                    << "** wait_until_taglist_clean_locked not work";
            }
        }
        lk.write_unlock();

        if (ret)
        {
            item_nr_.current()--;

            metric_.current().invalidate_put++;
            debug_record_action(key, *ret, Action::kInvalidate);
            backend_->put(key, *ret, ctx);
            {
                UniqueGuard guard(lk);
                // LOG(INFO) << "inv: C for req " << PRE(key) << " from " << id;
                CHECK(taglist_clear_and_notify_all(tag_list, key, id.val))
                    << "not found " << PRE(key) << " in tag_list";
            }
            debug_record_action(key, *ret, Action::kEvictDone);
        }

        return ret;
    }

    /**
     * Since it is an LRU-like cache, distinguishing between update and insert
     * does not make sense.
     * return:
     * - inserted: std::nullopt
     * - overwrite: the old value.
     */
    std::shared_ptr<V> put(const K &key, V &&value, CoroContext *ctx = nullptr)
    {
        metric_.current().write_nr++;

        auto tid = util::get_thread_id();
        compound_uint64_t id(tid, ctx ? ctx->coro_id() : kNotACoro);

        auto [lru, tag_list, lk] = locate(key);

        std::shared_ptr<V> ret;
        std::optional<EntryT> evict_entry;
        std::atomic<bool> *evict_ready{nullptr};

        // LOG(INFO) << "before put: " << PRE(key) << ", " << PRE(value)
        //           << PRE(lru);
        {
            UniqueGuard guard(lk);

            // put cancel any TagState::kFetching tags
            // but does not touch with any evicting tags

            auto [left, right] = taglist_locate_all(tag_list, key);
            for (auto it = left; it != right && it != tag_list.end();)
            {
                auto &tag = it->second;
                if (tag.state() == TagState::kFetching)
                {
                    it = do_taglist_clear_and_notify_all(tag_list, it);
                }
                else
                {
                    // it is evicting tag, do not touch
                    // just advance it
                    ++it;
                }
            }

            debug_record_action(key, value, Action::kLRUPut);

            auto inner_value = std::make_shared<V>(std::move(value));
            ret = lru.put(key, inner_value, false).value_or(nullptr);
            if (!ret)
            {
                // we inserted a new one
                item_nr_.current()++;
            }
            else
            {
                // key exists in the LRU
                // can not simultaneously in the tag_list
            }

            evict_entry = lru.evict();
            if (evict_entry)
            {
                debug_record_action(evict_entry->first,
                                    *evict_entry->second,
                                    Action::kLRUEvict);
                evict_ready = taglist_add(tag_list,
                                          evict_entry->first,
                                          TagState::kEvicting,
                                          id.val,
                                          ctx);
            }
        }

        if (evict_entry)
        {
            debug_record_tag("wait on put", evict_ready, id.val);
            wait_cv(evict_ready, ctx);

            item_nr_.current()--;

            auto &e = *evict_entry;
            // N.B. Can not use async here
            // because flood of eviction overwhelm the QP
            // TODO: change to prepare and commit API to enable batching

            metric_.current().write_evict++;
            debug_record_action(
                evict_entry->first, *evict_entry->second, Action::kEvictOnPut);

            backend_->put(e.first, *e.second, ctx);
            {
                UniqueGuard guard(lk);
                // may not success:
                // If concurrent put overwrites the evicting data.
                // LOG(INFO) << "put: C for evict: " << PRE(e.first) << " from "
                //           << id << ": " << PRE(tag_list);
                CHECK(taglist_clear_and_notify_all(tag_list, e.first, id.val));
            }
            debug_record_action(
                evict_entry->first, *evict_entry->second, Action::kEvictDone);
        }
        // LOG(INFO) << "after put: " << PRE(lru);

        return ret;
    }

    // We must copy the value as return,
    // because the item may be invalidated concurrently.
    std::shared_ptr<V> get(const K &key,
                           bool read_backend,
                           CoroContext *ctx = nullptr)
    {
        metric_.current().read_nr++;

        auto tid = util::get_thread_id();
        compound_uint64_t id(tid, ctx ? ctx->coro_id() : kNotACoro);

        auto [lru, tag_list, lk] = locate(key);

        std::shared_ptr<V> opt;  // enforce NRVO
        std::optional<EntryT> evict_entry;
        std::atomic<bool> *evict_ready{nullptr};

        // LOG(INFO) << "before get: " << PRE(lru);
        wait_until_taglist_clean_locked(tag_list, lk, key, ctx);
        {
            opt = bucket_locate_and_lru(lru, key).value_or(nullptr);

            if (opt == nullptr)
            {
                // locate failed:
                // data is in the backend
                if (read_backend)
                {
                    // ... and we need to read it
                    // put to front, because it is hotest
                    // tag_list.emplace_front(key, TagState::kFetching);
                    auto *cv = taglist_add(
                        tag_list, key, TagState::kFetching, id.val, ctx);
                    DCHECK_EQ(cv, nullptr)
                        << "** wait_until_taglist_clean_locked: can not "
                           "witness concurrent tags.";
                    // okay, go out and release lock
                    // when backend done, will go back and remove the tag
                }
                else
                {
                    // did not ask us to read the backend:
                    // do nothing
                    debug_record_action(key, *opt, Action::kLRUGet);
                }
            }
            else
            {
            }

            evict_entry = lru.evict();
            if (evict_entry)
            {
                debug_record_action(evict_entry->first,
                                    *evict_entry->second,
                                    Action::kLRUEvict);

                evict_ready = taglist_add(tag_list,
                                          evict_entry->first /* key */,
                                          TagState::kEvicting,
                                          id.val,
                                          ctx);
            }
        }
        lk.write_unlock();

        // NOTE: leverage batch API to reduce RTT
        // RESULT: the same performance
        V _be_value;
        if (opt == nullptr && read_backend)
        {
            _be_value = backend_->get_async(key, ctx);
        }
        if (evict_entry)
        {
            wait_cv(evict_ready, ctx);
            auto &e = evict_entry.value();
            backend_->put_async(e.first, *e.second, ctx);
        }
        backend_->sync(ctx);

        if (opt == nullptr && read_backend)
        {
            metric_.current().read_miss++;

            debug_record_action(key, _be_value, Action::kFetchOnGet);

            auto inner_value = std::make_shared<V>(std::move(_be_value));

            {
                UniqueGuard guard(lk);

                bool has_tag =
                    taglist_clear_and_notify_all(tag_list, key, id.val);
                if (has_tag)
                {
                    // normal path:
                    // we add the value into the LRU

                    debug_record_action(
                        key, *inner_value, Action::kLRUPutOnGet);

                    auto res = lru.put(key, inner_value, false);
                    CHECK(!res)
                        << "** Internal error: has_tag, but putting the key "
                           "into LRU also overwriten existing entry.";
                    item_nr_.current()++;
                }
                else
                {
                    // N.B.
                    // The tag disappears:
                    // It typically means that a write to the same key fill the
                    // LRU and remove the tag.

                    // do nothing
                }

                DCHECK_EQ(opt, nullptr);
                opt = inner_value;
            }
        }

        if (evict_entry)
        {
            debug_record_tag("wait on get", evict_ready, id.val);

            metric_.current().read_evict++;
            item_nr_.current()--;
            // must be sync
            // maybe yield here
            auto &e = evict_entry.value();
            debug_record_action(e.first, *e.second, Action::kEvictOnGet);

            {
                UniqueGuard guard(lk);
                // LOG(INFO) << "get: C " << PRE(e.first) << " from " << id;
                CHECK(taglist_clear_and_notify_all(tag_list, e.first, id.val));
            }

            debug_record_action(e.first, *e.second, Action::kEvictDone);
        }

        // LOG(INFO) << "after get: " << PRE(lru);
        return opt;
    }

    ssize_t size() const
    {
        auto sum_item_nr =
            item_nr_.accumulate([](ssize_t acc, const std::atomic<ssize_t> &cur)
                                { return acc + cur.load(); },
                                (ssize_t) 0);
        return sum_item_nr;
    }

    size_t bucket_evict_limit() const
    {
        return expect_bucket_limit_;
    }

    template <typename U = ICacheBackendT>
    U &get_backend()
    {
        return dynamic_cast<U &>(*backend_);
    }
    template <typename U = ICacheBackendT>
    const U &get_backend() const
    {
        return dynamic_cast<const U &>(*backend_);
    }

    ~ThreadSafeHashCache()
    {
        invalidate_all();
    }
    void invalidate_all()
    {
        for (size_t i = 0; i < bucket_nr_; ++i)
        {
            UniqueGuard guard(rw_locks_[i]);
            auto &lru = lrus_[i];
            while (!lru.empty())
            {
                metric_.current().invalidate_put++;

                auto evict_entry = lru.force_evict();
                CHECK(evict_entry.has_value());
                // because it is lock-guarded
                // it is okay to directly write through to the backend
                backend_->put(evict_entry->first,
                              *evict_entry->second,
                              nullptr /* ctx */);
            }
            lru.clear();
        }
    }
    void drop_all()
    {
        for (size_t i = 0; i < bucket_nr_; ++i)
        {
            UniqueGuard guard(rw_locks_[i]);
            auto &lru = lrus_[i];
            lru.clear();
            CHECK(lru.empty());
        }
    }
    auto &history()
    {
        return history_.current();
    }
    const auto &tl_history() const
    {
        return history_;
    }
    const auto &tl_tag_history() const
    {
        return tag_history_;
    }

    Perthread<CacheMetric> &metrics()
    {
        return metric_;
    }

private:
    size_t bucket_nr_;
    ssize_t item_limit_;

    AlignedVector<LRU> lrus_;
    ICacheBackendPtr backend_;
    AlignedVector<TagListT> tags_;
    mutable AlignedVector<util::RWLock> rw_locks_;

    ssize_t expect_bucket_limit_;
    Perthread<std::atomic<ssize_t>> item_nr_;

    // provided per-thread condition variable
    // used if coroutine is not enabled
    Perthread<std::atomic<bool>> condition_variable_;

    TL_History<ActionRecordT> history_;
    TL_History<TagRecord> tag_history_;

    Perthread<CacheMetric> metric_;

    __attribute__((always_inline)) void debug_record_action(
        const K &key, const V &value, Action act, std::list<uint32_t> &&ls = {})
    {
        if constexpr (kDebug)
        {
            history_.current().add(ActionRecordT{.key = key,
                                                 .value = value,
                                                 .act = act,
                                                 .wait_list = std::move(ls)});
        }
    }
    __attribute__((always_inline)) void debug_record_tag(const char *act,
                                                         std::atomic<bool> *cv,
                                                         uint64_t id)
    {
        if constexpr (kDebug)
        {
            tag_history_.current().add(
                TagRecord{.act = act,
                          .cv = cv,
                          .tid = (uint32_t) (id >> 32),
                          .cid = (uint32_t) (id << 32 >> 32)});
        }
    }

    std::tuple<LRU &, TagListT &, RWLock &> locate(const K &key)
    {
        auto hash_val = CityHash64((const char *) &key, sizeof(key));
        auto idx = hash_val % lrus_.size();
        auto &lk = rw_locks_[idx];
        auto &lru = lrus_[idx];
        auto &tag_list = tags_[idx];
        return {lru, tag_list, lk};
    }

    // pre-condition: bucket should be write locked
    // post-condition:
    // - If key exists:
    //   return optional(old_value)
    // - If key does not exists:
    //   return std::nullopt
    std::optional<InnerV> bucket_locate_and_lru(LRU &bucket, const K &key)
    {
        // bucket.get(k) automatically performs LRU operations
        return bucket.get(key);
    }

    std::optional<InnerV> bucket_invalidate(LRU &bucket, const K &key)
    {
        return bucket.invalidate(key);
    }

    typename TagListT::iterator taglist_locate(TagListT &tag_list,
                                               const K &key,
                                               uint64_t id)
    {
        auto [left, right] = tag_list.equal_range(key);
        for (auto it = left; it != right; ++it)
        {
            if (it->second.id() == id)
            {
                return it;
            }
        }
        return tag_list.end();
    }
    auto taglist_locate_all(TagListT &tag_list, const K &key)
    {
        return tag_list.equal_range(key);
    }

    /**
     * Add the decided operation to the tag list
     * @return
     * - nullptr: the operation is ready to do
     * - ptr: operation should not issue until {ptr->load(relaxed) == true}
     *
     */
    [[nodiscard]] std::atomic<bool> *taglist_add(TagListT &tag_list,
                                                 const K &key,
                                                 TagState state,
                                                 uint64_t id,
                                                 CoroContext *ctx)
    {
        auto it = tag_list.emplace(key, Tag(state, id));
        // find my direct predecessor
        // N.B. rit follows C++ reverse iterator rule:
        // use std::prev(it) when dereferencing
        auto rit = std::make_reverse_iterator(it);
        if (rit == tag_list.rend())
        {
            // no predecessor, no wait
            return nullptr;
        }
        else
        {
            const auto &tag_key = rit->first;
            auto &tag = rit->second;
            if (tag_key == key)
            {
                // is conflict: join and wait
                auto *cv = get_cv(ctx);
                tag.join(cv);

                debug_record_tag("tag join", cv, tag.id());

                return cv;
            }
            else
            {
                // no conflict, no wait
                return nullptr;
            }
        }
    }
    bool taglist_clear_and_notify_all(TagListT &tag_list,
                                      const K &key,
                                      uint64_t id)
    {
        auto [left, right] = tag_list.equal_range(key);
        bool found = false;
        for (auto it = left; it != right && it != tag_list.end();)
        {
            if (it->second.id() == id)
            {
                it = do_taglist_clear_and_notify_all(tag_list, it);
                found = true;
            }
            else
            {
                ++it;
            }
        }
        return found;
    }
    bool taglist_clear_any_and_notify_all(TagListT &tag_list, const K &key)
    {
        bool has_found = false;
        auto [left, right] = tag_list.equal_range(key);

        for (auto it = left; it != right && it != tag_list.end();)
        {
            it = do_taglist_clear_and_notify_all(tag_list, it);
            has_found = true;
        }
        return has_found;
    }
    typename TagListT::iterator do_taglist_clear_and_notify_all(
        TagListT &tag_list, typename TagListT::iterator it)
    {
        auto &tag = it->second;
        for (std::atomic<bool> *cv : tag.wait_set())
        {
            cv->store(true, std::memory_order_acq_rel);
            debug_record_tag("notify", cv, tag.id());
        }
        return tag_list.erase(it);
    }

    // wait until tag_list does not contain any entries of key
    // pre-condition: lock is NOT locked
    // post-condition: tag_list is clean with key and lock IS locked
    void wait_until_taglist_clean_locked(TagListT &tag_list,
                                         RWLock &lk,
                                         const K &key,
                                         CoroContext *ctx)
    {
        do
        {
            lk.write_lock();

            auto [left, right] = taglist_locate_all(tag_list, key);
            if (unlikely(left != right))
            {
                // taglist of key is not empty
                // wait until all concurrent operation finished

                // register myself to the *last* tag in the hope that
                // false wake up will be less

                right--;
                auto &tag = right->second;

                auto *cv = get_cv(ctx);
                debug_record_tag("join clean", cv, 0);
                tag.join(cv);

                lk.write_unlock();

                // wait until cv == true
                debug_record_tag("wait on clean", cv, 0);
                wait_cv(cv, ctx);
            }
            else
            {
                // post-condition satisfies
                return;
            }
        } while (true);
    }

    // get a conditional variable
    std::atomic<bool> *get_cv(CoroContext *ctx)
    {
        std::atomic<bool> *ret;
        if (ctx)
        {
            ret = ctx->wait_variable();
        }
        else
        {
            ret = &(condition_variable_.current());
        }
        DCHECK_NOTNULL(ret)->store(false, std::memory_order_acq_rel);
        debug_record_tag("reset get_cv", ret, 0);
        return ret;
    }
    __attribute__((always_inline)) void wait_cv(std::atomic<bool> *cv,
                                                CoroContext *ctx)
    {
        if (cv == nullptr)
        {
            return;
        }
        if (cv->load(std::memory_order_relaxed))
        {
            // so lucky that it becomes ready very soon
            // no need to pay for scheduling.
            return;
        }
        if (ctx)
        {
            ctx->wait_yield_to_master();
        }
        else
        {
            // reduce contention from rwlock
            // to contention of a single variable
            while (!cv->load(std::memory_order_acq_rel))
            {
                util::asms::cpu_relax();
            }
        }
        DCHECK(cv->load(std::memory_order_acq_rel));
        debug_record_tag("wait leave", cv, 0);
    }
};

inline std::ostream &operator<<(std::ostream &os, const CacheMetric &m)
{
    os << "{Metric: ";
    size_t total_op = m.total_op();
    size_t total_io = m.total_io();
    if (m.read_nr)
    {
        double io_rate = 1.0 * (m.read_miss + m.read_evict) / m.read_nr;
        os << "read: " << util::pre_num(m.read_nr) << ", miss "
           << util::pre_num(m.read_miss)
           << ", evict: " << util::pre_num(m.read_evict) << "("
           << util::pre_pcnt(io_rate) << "); ";
    }
    if (m.write_nr)
    {
        double io_rate = 1.0 * m.write_evict / m.write_nr;
        os << "write: " << util::pre_num(m.write_nr)
           << ", evict: " << util::pre_num(m.write_evict) << " ("
           << util::pre_pcnt(io_rate) << "); ";
    }
    if (m.invalidate_nr)
    {
        double io_rate = 1.0 * m.invalidate_put / m.invalidate_nr;
        os << "inv: " << util::pre_num(m.invalidate_nr)
           << ", put: " << util::pre_num(m.invalidate_put) << " ("
           << util::pre_pcnt(io_rate) << "); ";
    }
    os << "total io_rate: " << util::pre_pcnt(1.0 * total_io / total_op);
    return os;
}

}  // namespace util::hash