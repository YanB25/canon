#include <numa.h>

#include <thread>

#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/History.h"
#include "util/ThreadSafeHashCache.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"

constexpr static size_t kBucketNr = 10;
constexpr static size_t kCacheSize = 2 * kBucketNr;  // each bucket length
constexpr static uint64_t kKeyRng = 100 * kCacheSize;
// constexpr static uint64_t kMaxKey = kKeyRng * (1 + kMaxAppThread);
static_assert(kKeyRng >= kCacheSize);

using K = uint64_t;
using V = uint64_t;

struct Record
{
    K key;
    V expect_v;
    V actual_v;
};
inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::Record &v)
{
    os << "{Record ";
    os << "key: " << util::pre(v.key);
    os << ", expect: " << util::pre(v.expect_v);
    os << ", actual: " << util::pre(v.actual_v);
    os << "}";
    return os;
}
struct Spec
{
    struct TLS
    {
        util::ZipfianGenerator g{0, kKeyRng - 1};
        std::map<uint64_t, uint64_t> map;
        std::map<uint64_t, uint64_t> evict_record;
        util::History<std::pair<K, V>> history;
        uint32_t self_idx = 0;
    };
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    using BackendT = util::MapBackend<uint64_t, uint64_t>;
    using BLS = typename Base::BLS;
    using TLS = typename Base::TLS;

    Experiment()
    {
    }
    __attribute__((always_inline)) uint64_t gen_self_key(TLS &tlc)
    {
        std::ignore = tlc;
        {
            // uniform version
            // much easier to find potential bugs
            auto tid = util::get_thread_id();
            uint64_t min = kKeyRng * tid;
            // use uniform distribution
            // so that it is easier to catch bugs
            return fast_pseudo_rand_int(min, min + kKeyRng - 10);
        }
        // {
        //     auto tid = util::get_thread_id();
        //     uint64_t min = kKeyRng * tid;
        //     auto k = tlc.g.Next();
        //     CHECK_LT(k, kKeyRng);
        //     return min + k;
        // }
    }
    uint64_t self_min()
    {
        return kKeyRng * util::get_thread_id();
    }
    uint64_t self_max()
    {
        return kKeyRng * (util::get_thread_id() + 1) - 1;
    }
    uint64_t gen_self_value(TLS &tlc, uint64_t)
    {
        uint64_t tid = util::get_thread_id();
        uint64_t self_value = (tid << 32) + tlc.self_idx++;
        return self_value;
    }
    __attribute__((always_inline)) int choose_action(size_t read_rate,
                                                     size_t evict_rate)
    {
        if (fast_pseudo_bool_with_prob(read_rate / 100.0))
        {
            return 0;  // read
        }
        if (fast_pseudo_bool_with_prob(evict_rate / 100.0))
        {
            return 2;  // evict
        }
        return 1;  // insert
    }
    void check_data_consistent(uint64_t key, uint64_t expect_val)
    {
        auto cache_val = cache_->get(key, true);
        if (cache_val)
        {
            // auto v = cache_val.value();
            const auto &v = *cache_val;
            LOG_IF(WARNING, v != expect_val)
                << "value mismatch for " << PRE(key)
                << ". tid: " << util::get_thread_id() << ". expect tid "
                << (expect_val >> 32)
                << ", expect idx: " << (expect_val << 32 >> 32)
                << ", actual tid: " << (v >> 32)
                << ", actual idx: " << (v << 32 >> 32);
            if (v != expect_val)
            {
                std::lock_guard<std::mutex> lk(mu_);
                error_record_.push_back(Record{
                    .key = key,
                    .expect_v = expect_val,
                    .actual_v = v,
                });
            }
        }
        else
        {
            LOG(FATAL) << PRE(key) << " disappear.";
        }
    }
    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        auto &tlc = bls.thread();

        auto tid = util::get_thread_id();
        CHECK_LT(tid, kMaxAppThread);
        auto read_rate = conf.get<size_t>("read_rate");
        auto evict_rate = conf.get<size_t>("evict_rate");
        CHECK_LE(read_rate, 100);
        CHECK_LE(evict_rate, 100);
        while (likely(!token->stop_requested()))
        {
            int action = choose_action(read_rate, evict_rate);
            auto self_key = gen_self_key(tlc);
            op_nr_.current()++;

            if (action == 0)
            {
                // read
                auto value = cache_->get(self_key, false);
                // VLOG(1) << PRE(tid) << ": get " << PRE(self_key);
                if (value)
                {
                    CHECK(tlc.map.count(self_key));
                    CHECK_EQ(*value, tlc.map[self_key]);
                }
                else
                {
                    // miss
                    miss_nr_.current()++;
                    tlc.evict_record[self_key]++;
                }
            }
            else if (action == 1)
            {
                // insert
                auto value = gen_self_value(tlc, self_key);

                // record self history to debug
                tlc.history.add(std::make_pair(self_key, value));

                auto ins = cache_->put(self_key, std::move(value));
                if (ins)
                {
                    // overwrite
                    CHECK(tlc.map.count(self_key));
                    CHECK_EQ(*ins, tlc.map[self_key]);
                }
                else
                {
                    miss_nr_.current()++;
                    tlc.evict_record[self_key]++;
                }
                tlc.map[self_key] = value;
            }
            else
            {
                CHECK_EQ(action, 2);
                // invalidate
                auto old_entry = cache_->invalidate(self_key);
                // VLOG(1) << PRE(tid) << ": invalidate " << PRE(self_key);
                if (old_entry)
                {
                    CHECK(tlc.map.count(self_key));
                    CHECK_EQ(*old_entry, tlc.map[self_key]);
                }
                else
                {
                    miss_nr_.current()++;
                    tlc.evict_record[self_key]++;
                }
            }
            token->complete_task(1);
        }
        for (const auto &[k, v] : tlc.map)
        {
            auto maybe_v = cache_->invalidate(k);
            if (maybe_v)
            {
                CHECK_EQ(*maybe_v, v);
            }
        }
    }
    void on_thread_start_bench(BLS &bls, const Config &conf) override
    {
        auto &tlc = bls.thread();
        // preload
        tlc.map.clear();
        // LOG(INFO) << "** preloading [" << self_min() << ", " << self_max()
        //           << "]";
        static std::mutex loading_mu;
        {
            // this mutex is not necessary: the backend is thread safe
            // however, it speeds up the loading process by reducing contention
            std::lock_guard<std::mutex> lock(loading_mu);

            for (size_t i = self_min(); i <= self_max(); ++i)
            {
                auto self_key = i;
                auto value = gen_self_value(tlc, self_key);
                auto old_val = cache_->put(self_key, std::move(value));
                tlc.map[self_key] = value;
            }
        }

        Base::on_thread_start_bench(bls, conf);
    }
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        LOG(INFO) << PRE(conf);
        cache_ = std::make_unique<CacheT>(
            BackendT::make_ptr(), kBucketNr, kCacheSize);
        error_record_.clear();

        Base::on_start_bench(bls, conf);
    }
    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        auto &tlc = bls.thread();
        // LOG(INFO) << PRE(tlc.map.size());
        // LOG(INFO) << PRE(tlc.evict_record);

        // each entry in the map either
        // - exists in cache, or
        // - exists in backend
        // they must be found
        for (const auto &[k, v] : tlc.map)
        {
            check_data_consistent(k, v);
            // evict any item from cache to the beckend
            // In any case the evict causes eviction
            // delete that records
        }

        // give information of error
        if (!error_record_.empty())
        {
            std::lock_guard<std::mutex> lk(mu_);

            const auto &err = error_record_.front();

            using namespace ranges;

            auto last = tlc.history.history() |
                        ranges::views::filter([&err](const auto &e)
                                              { return e.first == err.key; }) |
                        ranges::views::take(10) | to<std::vector>();

            LOG_IF(INFO, !last.empty())
                << "FROM thread " << util::get_thread_id() << ": " << PRE(last);
        }

        Base::on_thread_end_bench(bls, conf);
    }
    void on_end_bench(const ::bench::ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        LOG(INFO) << "on_end_bench.";
        auto f = [](size_t acc, size_t cur) { return acc + cur; };
        size_t total_op = op_nr_.accumulate(f, 0ull);
        size_t miss_nr = miss_nr_.accumulate(f, 0ull);

        auto &be = cache_->get_backend<BackendT>();
        LOG(INFO) << "in cache: " << cache_->size()
                  << ", evicted unique entry: " << be.map().size();
        LOG(INFO) << "total: " << total_op << ", miss: " << miss_nr
                  << ", miss rate: "
                  << util::pre_pcnt(1.0 * miss_nr / total_op);

        if (!error_record_.empty())
        {
            const auto &err = error_record_.front();
            using namespace ranges;
            auto his = be.history().history() |
                       views::filter([&err](const auto &r)
                                     { return r.first == err.key; }) |
                       views::take(10) | to<std::vector>();

            LOG_IF(WARNING, !error_record_.empty())
                << PRE(error_record_.front());
            LOG_IF(WARNING, !his.empty()) << "From Backend: " << PRE(his);

            const auto &tl_his = cache_->tl_history().take(20);

            LOG_IF(WARNING, !tl_his.empty()) << "From cache: " << PRE(tl_his);
        }
        CHECK(error_record_.empty()) << "** Check failed";

        cache_.reset();
        error_record_.clear();
        Base::on_end_bench(res, bls, conf);
    }

private:
    using CacheT = util::hash::ThreadSafeHashCache<K, V>;

    std::unique_ptr<CacheT> cache_;
    std::vector<Record> error_record_;
    std::mutex mu_;

    // used to report cache hit (miss) rate
    Perthread<size_t> op_nr_;
    Perthread<size_t> miss_nr_;
};

void simple()
{
    using CacheT = util::hash::ThreadSafeHashCache<uint64_t, uint64_t>;

    using BackendT = util::MapBackend<uint64_t, uint64_t>;
    auto cache = std::make_unique<CacheT>(
        BackendT::make_ptr(), 1 /* bucket nr*/, 10 /* cache size */);

    for (uint64_t key = 0; key < 100; ++key)
    {
        auto value = key;
        cache->put(key, std::move(value));
    }
    for (uint64_t key = 0; key < 100; ++key)
    {
        cache->put(key, 2 * key);
    }

    for (uint64_t key = 0; key < 100; ++key)
    {
        auto e = cache->invalidate(key);
    }
}

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    // simple();

    Experiment exp;
    exp.configure_monitor(100ms, 20);
    ::bench::ConfigFactory f;

    {
        f.configure_thread_nr({1, 4, 18, kMaxAppThread});
        f.add_option<size_t>("read_rate", {100, 50, 0});
        f.add_option<size_t>("evict_rate", {50, 0});
    }

    // {
    //     f.configure_thread_nr({18});
    //     f.add_option<size_t>("read_rate", {0});
    //     f.add_option<size_t>("evict_rate", {0});
    // }

    exp.launch(f.generate_configs());

    LOG(INFO) << "PASS.";
}