#pragma once

#include <city.h>

#include <atomic>
#include <functional>
#include <iostream>

#include "Common.h"
#include "CoroContext.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "IOVerbose.h"
#include "bench/token.h"
#include "sherman/PageCache.h"
#include "sherman/TreeConfig.h"

namespace sherman
{
extern uint64_t cache_miss[kMaxAppThread][8];
extern uint64_t cache_hit[kMaxAppThread][8];
extern bool enter_debug;
extern uint64_t latency[kMaxAppThread][LATENCY_WINDOWS];

extern thread_local std::shared_ptr<avis::AvisHandle>
    avis_handle[define::kMaxCoroNr];
extern thread_local std::shared_ptr<avis::AvisHandle> master_avis_handle;

class IndexCache;
struct LocalLockNode
{
    std::atomic<uint64_t> ticket_lock;
    bool hand_over;
    uint8_t hand_time;
};

struct Request
{
    bool is_search;
    size_t value_size;
    Key k;
    Value v;
};

class RequstGen
{
public:
    RequstGen() = default;
    virtual Request next()
    {
        return Request{};
    }
    virtual ~RequstGen() = default;
};

using CoroFunc = std::function<RequstGen *(int, DSM *, int)>;

struct SearchResult
{
    bool is_leaf;
    uint8_t level;
    GlobalAddress slibing;
    GlobalAddress next_level;
    Value val;
};

inline std::ostream &operator<<(
    std::ostream &os, [[maybe_unused]] const ::sherman::SearchResult &v)
{
    os << "{SearchResult ";
    os << "is_leaf: " << util::pre(v.is_leaf);
    os << ", level: " << util::pre(v.level);
    os << ", slibing: " << util::pre(v.slibing);
    os << ", next_level: " << util::pre(v.next_level);
    os << ", val: " << util::pre(v.val);
    os << "}";
    return os;
}

class AllocCache
{
public:
    template <typename AllocFn>
    void prepare(AllocFn &&alloc)
    {
        while (cache_[kLeafPageSize].size() < 6)
        {
            auto ret = alloc(kLeafPageSize);
            CHECK(!ret.is_null());
            cache_[kLeafPageSize].push_back(ret);
        }
        while (cache_[kLeafPageSize].size() > 6)
        {
            auto addr = cache_[kLeafPageSize].back();
            cache_[kLeafPageSize].pop_back();
            free(addr, kLeafPageSize);
        }
        while (cache_[kInternalPageSize].size() < 6)
        {
            auto ret = alloc(kInternalPageSize);
            CHECK(!ret.is_null());
            cache_[kInternalPageSize].push_back(ret);
        }
        while (cache_[kInternalPageSize].size() > 6)
        {
            auto addr = cache_[kInternalPageSize].back();
            cache_[kInternalPageSize].pop_back();
            free(addr, kInternalPageSize);
        }
    }
    GlobalAddress alloc(size_t size)
    {
        CHECK(!cache_[size].empty()) << "** run out of cache for size " << size;
        auto ret = cache_[size].back();
        cache_[size].pop_back();
        CHECK(!ret.is_null());
        return ret;
    }
    void free(GlobalAddress gaddr, size_t size)
    {
        cache_[size].push_back(gaddr);
    }

private:
    std::map<size_t, std::vector<GlobalAddress>> cache_;
};

class InternalPage;
class LeafPage;
class Tree
{
public:
    constexpr static size_t V = ::config::verbose::kUserApp_1;
    using pointer = std::shared_ptr<Tree>;

    Tree(std::shared_ptr<DSM> dsm,
         GlobalAddress tree_meta,
         const TreeConfig &conf,
         size_t server_nid);
    static pointer new_instance(DSM::pointer dsm,
                                GlobalAddress tree_meta,
                                const TreeConfig &conf,
                                size_t server_nid)
    {
        return std::make_shared<Tree>(
            CHECK_NOTNULL(dsm), tree_meta, conf, server_nid);
    }
    void reset_config(const TreeConfig &conf)
    {
        c_ = conf;
        maybe_reset_page_cache(conf.bucket_nr, conf.cache_limit);
    }
    avis::AvisHandle *get_avis_handle(CoroContext *ctx);

    void insert(const Key &k, const Value &v, CoroContext *cxt = nullptr);
    bool search(const Key &k, Value &v, CoroContext *cxt = nullptr);
    void del(const Key &k, CoroContext *cxt = nullptr);

    std::pair<size_t, size_t> print_and_check_tree(CoroContext *cxt = nullptr);
    void print_path_stack(Key key, CoroContext *cxt, int coro_id);

    void run_coroutine(CoroFunc func,
                       int id,
                       int coro_cnt,
                       bool is_master,
                       ::bench::StopToken::pointer);
    void run_coroutine_lock_bench(CoroFunc func,
                                  int id,
                                  int coro_cnt,
                                  bool is_master,
                                  ::bench::StopToken::pointer stop_token);

    void lock_bench(const Key &k, CoroContext *cxt = nullptr, int coro_id = 0);

    GlobalAddress query_cache(const Key &k);
    void index_cache_statistics();
    void clear_statistics();

    using StopToken = ::bench::StopToken;

    PageCache &page_cache()
    {
        return *page_cache_;
    }
    void maybe_reset_page_cache(size_t bucket_nr, size_t cache_limit)
    {
        bool need_reset = (page_cache_ == nullptr) ||
                          (page_cache_->bucket_nr() != bucket_nr) ||
                          (page_cache_->cache_limit() != cache_limit);
        if (need_reset)
        {
            LOG(INFO) << "Reset page cache: " << PRE(bucket_nr) << ", "
                      << PRE(cache_limit);
            page_cache_ =
                std::make_unique<PageCache>(dsm, bucket_nr, cache_limit);
        }
    }
    constexpr std::pair<int64_t, int64_t> cache_statistics() const
    {
        auto sum_cache_hit = cache_hit_.accumulate(
            [](auto lhs, auto rhs) { return lhs + rhs; }, (int64_t) 0);
        auto sum_cache_miss = cache_miss_.accumulate(
            [](auto lhs, auto rhs) { return lhs + rhs; }, (int64_t) 0);
        return {sum_cache_hit, sum_cache_miss};
    }

    // when in critical path, especially with lock held
    // use fast_* API
    void fast_free(GlobalAddress gaddr, size_t size, CoroContext *);
    GlobalAddress fast_alloc(size_t size, [[maybe_unused]] CoroContext *ctx);

    GlobalAddress do_alloc(size_t size, [[maybe_unused]] CoroContext *ctx);
    void do_free(GlobalAddress gaddr, size_t size, CoroContext *);

    void prepare_alloc(CoroContext *ctx);

    ~Tree();

    static thread_local std::queue<uint16_t> hot_wait_queue;

    void register_avis_handle(CoroContext *ctx);
    void reset_avis_handle(CoroContext *ctx);

private:
    std::shared_ptr<DSM> dsm;
    GlobalAddress tree_meta_;
    TreeConfig c_;
    GlobalAddress root_ptr_ptr;  // the address which stores root pointer;
    std::unique_ptr<PageCache> page_cache_;
    size_t server_nid_;

    Perthread<int64_t> cache_hit_;
    Perthread<int64_t> cache_miss_;

    // static thread_local int coro_id;
    static thread_local CoroCall worker[define::kMaxCoroNr];
    static thread_local CoroCall master;

    static thread_local bool coro_finished[define::kMaxCoroNr];
    static thread_local size_t coro_finished_nr;

    static thread_local AllocCache alloc_cache_[define::kMaxCoroNr];

    // [MAX_MACHINE][kNumOfLock]
    LocalLockNode *local_locks[MAX_MACHINE];

    IndexCache *index_cache;

    void print_verbose();

    void before_operation(CoroContext *cxt, int coro_id);

    GlobalAddress get_root_ptr_ptr();
    GlobalAddress get_root_ptr(CoroContext *cxt, int coro_id);

    void coro_worker(RequstGen *gen,
                     CoroContext &ctx,
                     bool is_master,
                     StopToken::pointer);
    void coro_worker_bench_lock(RequstGen *gen,
                                CoroContext &ctx,
                                bool is_master,
                                StopToken::pointer);
    void coro_master(CoroContext &ctx,
                     int coro_cnt,
                     bool is_master,
                     StopToken::pointer);

    void broadcast_new_root(GlobalAddress new_root_addr, int root_level);
    bool update_new_root(GlobalAddress left,
                         const Key &k,
                         GlobalAddress right,
                         int level,
                         GlobalAddress old_root,
                         CoroContext *cxt,
                         int coro_id);

    void insert_internal(const Key &k,
                         GlobalAddress v,
                         CoroContext *cxt,
                         int coro_id,
                         int level);

    bool try_lock_addr(GlobalAddress lock_addr,
                       uint64_t tag,
                       uint64_t *buf,
                       CoroContext *cxt,
                       int coro_id);
    void unlock_addr(GlobalAddress lock_addr,
                     uint64_t tag,
                     uint64_t *buf,
                     CoroContext *cxt,
                     int coro_id,
                     bool async);
    void write_page_and_unlock(util::Page &&page,
                               GlobalAddress page_addr,
                               off_t page_offset,
                               size_t size,
                               uint64_t *cas_buffer,
                               GlobalAddress lock_addr,
                               uint64_t tag,
                               CoroContext *cxt,
                               int coro_id,
                               bool async);

    std::shared_ptr<util::Page> lock_and_read_page_no_modify(
        GlobalAddress page_addr,
        int page_size,
        uint64_t *cas_buffer,
        GlobalAddress lock_addr,
        uint64_t tag,
        CoroContext *cxt,
        int coro_id);

    bool page_search(GlobalAddress page_addr,
                     const Key &k,
                     SearchResult &result,
                     CoroContext *cxt,
                     int coro_id,
                     bool from_cache = false);
    void internal_page_search(const InternalPage *page,
                              const Key &k,
                              SearchResult &result);
    void leaf_page_search(const LeafPage *page,
                          const Key &k,
                          SearchResult &result);

    void internal_page_store(GlobalAddress page_addr,
                             const Key &k,
                             GlobalAddress value,
                             GlobalAddress root,
                             int level,
                             CoroContext *cxt,
                             int coro_id);
    bool leaf_page_store(GlobalAddress page_addr,
                         const Key &k,
                         const Value &v,
                         GlobalAddress root,
                         int level,
                         CoroContext *cxt,
                         int coro_id,
                         bool from_cache = false);
    bool leaf_page_del(GlobalAddress page_addr,
                       const Key &k,
                       int level,
                       CoroContext *cxt,
                       int coro_id,
                       bool from_cache = false);

    bool acquire_local_lock(GlobalAddress lock_addr,
                            CoroContext *cxt,
                            int coro_id);
    bool can_hand_over(GlobalAddress lock_addr);
    void releases_local_lock(GlobalAddress lock_addr);
};

class Header
{
private:
    GlobalAddress leftmost_ptr;
    GlobalAddress sibling_ptr;
    uint8_t level;
    int16_t last_index;
    Key lowest;
    Key highest;

    friend class InternalPage;
    friend class LeafPage;
    friend class Tree;
    friend class IndexCache;

public:
    friend std::ostream &operator<<(std::ostream &, const Header &);
    Header()
    {
        leftmost_ptr = GlobalAddress::Null();
        sibling_ptr = GlobalAddress::Null();
        last_index = -1;
        lowest = kKeyMin;
        highest = kKeyMax;
    }
    bool is_leaf() const
    {
        return leftmost_ptr.is_null();
    }
    bool is_internal() const
    {
        return !is_leaf();
    }

    void debug() const
    {
        std::cout << "leftmost=" << leftmost_ptr << ", "
                  << "sibling=" << sibling_ptr << ", "
                  << "level=" << (int) level << ","
                  << "cnt=" << last_index + 1 << ","
                  << "range=[" << lowest << " - " << highest << "]";
    }
} __attribute__((packed));

inline std::ostream &operator<<(std::ostream &os, const Header &h)
{
    os << "{hdr: l_ptr: " << h.leftmost_ptr << ", sib: " << h.sibling_ptr
       << ", level: " << (int) h.level << ", last_idx: " << (int) h.last_index
       << ", lowest: " << h.lowest << ", highest: " << h.highest << "}";
    return os;
}

class InternalEntry
{
public:
    Key key;
    GlobalAddress ptr;

    InternalEntry()
    {
        ptr = GlobalAddress::Null();
        key = 0;
    }
} __attribute__((packed));
inline std::ostream &operator<<(std::ostream &os, const InternalEntry &e)
{
    os << "(" << e.key << ", " << e.ptr << ")";
    return os;
}

class LeafEntry
{
public:
    uint8_t f_version_;
    Key key;
    Value value;
    uint8_t r_version_;

    std::atomic<uint8_t> &f_version()
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &f_version_;
        return atm;
    }
    const std::atomic<uint8_t> &f_version() const
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &f_version_;
        return atm;
    }
    std::atomic<uint8_t> &r_version()
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &r_version_;
        return atm;
    }
    const std::atomic<uint8_t> &r_version() const
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &r_version_;
        return atm;
    }

    LeafEntry()
    {
        f_version() = 0;
        r_version() = 0;
        value = kValueNull;
        key = 0;
    }
} __attribute__((packed));

constexpr int kInternalCardinality =
    (kInternalPageSize - sizeof(Header) - sizeof(uint8_t) * 2) /
    sizeof(InternalEntry);

constexpr int kLeafCardinality =
    (kLeafPageSize - sizeof(Header) - sizeof(uint8_t) * 2) / sizeof(LeafEntry);

inline std::ostream &operator<<(std::ostream &os, const LeafEntry &e)
{
    os << "(" << e.key << ", " << e.value << ")";
    return os;
}

class InternalPage
{
    union
    {
        uint32_t crc;
        uint64_t embedding_lock;
        uint64_t index_cache_freq;
    };

    uint8_t front_version_;
    Header hdr;
    InternalEntry records[kInternalCardinality];

    [[maybe_unused]] uint8_t padding[3];
    uint8_t rear_version_;

    friend class Tree;
    friend class IndexCache;

public:
    std::atomic<uint8_t> &f_version()
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &front_version_;
        return atm;
    }
    std::atomic<uint8_t> &r_version()
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &rear_version_;
        return atm;
    }
    const std::atomic<uint8_t> &f_version() const
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &front_version_;
        return atm;
    }
    const std::atomic<uint8_t> &r_version() const
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &rear_version_;
        return atm;
    }
    std::pair<uint8_t, uint8_t> version() const
    {
        return {f_version(), r_version()};
    }
    friend std::ostream &operator<<(std::ostream &os, const InternalPage &p);
    // this is called when tree grows
    InternalPage(GlobalAddress left,
                 const Key &key,
                 GlobalAddress right,
                 uint32_t level = 0)
    {
        hdr.leftmost_ptr = left;
        hdr.level = level;
        records[0].key = key;
        records[0].ptr = right;
        records[1].ptr = GlobalAddress::Null();

        hdr.last_index = 0;

        f_version() = 0;
        r_version() = 0;
    }

    InternalPage(uint32_t level = 0)
    {
        hdr.level = level;
        records[0].ptr = GlobalAddress::Null();

        f_version() = 0;
        r_version() = 0;

        embedding_lock = 0;
    }

    void set_consistent()
    {
        f_version()++;
        r_version() = f_version().load();
#ifdef CONFIG_ENABLE_CRC
        this->crc = CityHash32((char *) &front_version,
                               (&rear_version) - (&front_version));
#endif
    }

    bool check_consistent() const
    {
        bool succ = true;
#ifdef CONFIG_ENABLE_CRC
        auto cal_crc = CityHash32((char *) &front_version,
                                  (&rear_version) - (&front_version));
        succ = cal_crc == this->crc;
#endif
        succ = succ && (r_version().load() == f_version().load());
        if (!succ)
        {
            // this->debug();
        }
        return succ;
    }

    void debug() const
    {
        std::cout << "InternalPage@ ";
        hdr.debug();
        std::cout << "version: [" << (int) f_version().load() << ", "
                  << (int) r_version().load() << "]" << std::endl;
    }

    void verbose_debug() const
    {
        this->debug();
        for (int i = 0; i < this->hdr.last_index + 1; ++i)
        {
            printf(
                "[%lu %lu] ", this->records[i].key, this->records[i].ptr.val);
        }
        printf("\n");
    }

} __attribute__((packed));

struct pre_leaf_page;
class LeafPage
{
private:
    union
    {
        uint32_t crc;
        uint64_t embedding_lock;
    };
    uint8_t front_version_;
    Header hdr;
    LeafEntry records[kLeafCardinality];

    [[maybe_unused]] uint8_t padding[1];
    uint8_t rear_version_;

    friend class Tree;
    friend std::ostream &operator<<(std::ostream &, const pre_leaf_page &);

public:
    std::atomic<uint8_t> &front_version()
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &front_version_;
        return atm;
    }
    const std::atomic<uint8_t> &front_version() const
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &front_version_;
        return atm;
    }
    std::atomic<uint8_t> &rear_version()
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &rear_version_;
        return atm;
    }
    const std::atomic<uint8_t> &rear_version() const
    {
        std::atomic<uint8_t> &atm = *(std::atomic<uint8_t> *) &rear_version_;
        return atm;
    }
    LeafPage(uint32_t level = 0)
    {
        hdr.level = level;
        records[0].value = kValueNull;

        front_version() = 0;
        rear_version() = 0;

        embedding_lock = 0;
    }
    std::pair<uint8_t, uint8_t> version() const
    {
        return {front_version(), rear_version()};
    }

    void set_consistent()
    {
        front_version()++;
        rear_version().store(front_version().load());
#ifdef CONFIG_ENABLE_CRC
        this->crc = CityHash32((char *) &front_version,
                               (&rear_version) - (&front_version));
#endif
    }

    bool check_consistent() const
    {
        bool succ = true;
#ifdef CONFIG_ENABLE_CRC
        auto cal_crc = CityHash32((char *) &front_version,
                                  (&rear_version) - (&front_version));
        succ = cal_crc == this->crc;
#endif

        succ = succ && (rear_version().load() == front_version().load());
        if (!succ)
        {
            LOG(WARNING) << "header: " << hdr
                         << ", version: " << (uint64_t) rear_version() << " vs "
                         << (uint64_t) front_version();
        }

        return succ;
    }

    void debug() const
    {
        std::cout << "LeafPage@ ";
        hdr.debug();
        std::cout << "version: [" << (int) front_version() << ", "
                  << (int) rear_version() << "]" << std::endl;
    }

} __attribute__((packed));

struct pre_leaf_page
{
    pre_leaf_page(const LeafPage &p, bool v = false) : p_(p), v_(v)
    {
    }
    const LeafPage &p_;
    bool v_;
};

inline std::ostream &operator<<(std::ostream &os, const pre_leaf_page &p)
{
    os << "{Page hdr: " << p.p_.hdr;
    if (p.v_)
    {
        os << ", records: " << util::pre(p.p_.records);
    }
    os << "}";
    return os;
}
inline std::ostream &operator<<(std::ostream &os, const InternalPage &p)
{
    os << "{Internal page: hdr: " << p.hdr
       << ", records: " << util::pre(p.records, 10) << "}";
    return os;
}

}  // namespace sherman