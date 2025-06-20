#include <numa.h>

#include <thread>

#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Page.h"
#include "util/ThreadSafeHashCache.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"

constexpr static size_t kBucketNr = 1024;
constexpr static size_t kCacheSize = 8 * kBucketNr;  // each bucket length
constexpr static uint64_t kMaxKey = 16_M;

using K = uint64_t;
using V = util::Page;
// using V = uint64_t;

struct ThreadContext
{
    util::ZipfianGenerator g{0, kMaxKey};
};

struct Spec
{
    using TLS = ThreadContext;
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    using BackendT = util::EmptyBackend<K, V>;
    using BLS = typename Base::BLS;
    using TLS = typename BLS::TLS;

    Experiment()
    {
    }
    __attribute__((always_inline)) int choose_action(size_t read_rate)
    {
        if (fast_pseudo_bool_with_prob(read_rate / 100.0))
        {
            return 0;  // read
        }
        return 1;  // insert
    }
    V gen_value(TLS &, const K &)
    {
        return V{kInternalPageSize};
    }
    void benchmark(BLS &s,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        auto &tls = s.thread();

        auto tid = util::get_thread_id();
        CHECK_LT(tid, kMaxAppThread);
        auto read_rate = conf.get<size_t>("read_rate");
        CHECK_LE(read_rate, 100);
        while (likely(!token->stop_requested()))
        {
            int action = choose_action(read_rate);
            auto self_key = tls.g.Next();

            op_nr_.current()++;

            if (action == 0)
            {
                // read
                auto value = cache_->get(self_key, false);
                if (!value)
                {
                    miss_nr_.current()++;
                    cache_->get(self_key, true);
                }
            }
            else if (action == 1)
            {
                // insert
                auto value = gen_value(tls, self_key);
                auto ins = cache_->put(self_key, std::move(value));
                if (!ins)
                {
                    miss_nr_.current()++;
                }
            }
            else
            {
                LOG(FATAL) << "** unknown action " << action;
            }
            token->complete_task(1);
        }
    }
    void on_thread_start_bench(BLS &s, const Config &conf) override
    {
        auto &tls = s.thread();
        auto total_thread_nr = conf.thread_nr();
        auto tid = util::get_thread_id();

        for (size_t i = 0; i <= kMaxKey; ++i)
        {
            if (i % total_thread_nr == tid)
            {
                auto self_key = i;
                auto value = gen_value(tls, self_key);
                cache_->put(self_key, std::move(value));
            }
        }

        Base::on_thread_start_bench(s, conf);
    }
    void on_start_bench(BLS &s, const Config &conf) override
    {
        cache_ = std::make_unique<CacheT>(
            BackendT::make_ptr(), kBucketNr, kCacheSize);

        Base::on_start_bench(s, conf);
    }

    void on_end_bench(const ::bench::ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        LOG(INFO) << "on_end_bench.";
        auto f = [](const size_t &acc, const size_t &cur) -> size_t {
            return acc + cur;
        };
        size_t total_op = op_nr_.accumulate(f, 0ull);
        size_t miss_nr = miss_nr_.accumulate(f, 0ull);

        auto &be = cache_->get_backend<BackendT>();
        LOG(INFO) << "in cache: " << cache_->size()
                  << ", miss number: " << be.miss_nr();
        LOG(INFO) << "total: " << total_op << ", miss: " << miss_nr
                  << ", miss rate: "
                  << util::pre_pcnt(1.0 * miss_nr / total_op);

        cache_.reset();
        Base::on_end_bench(res, bls, conf);
    }

private:
    using CacheT = util::hash::ThreadSafeHashCache<K, V>;

    std::unique_ptr<CacheT> cache_;
    // used to report cache hit (miss) rate
    Perthread<size_t> op_nr_;
    Perthread<size_t> miss_nr_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    Experiment exp;
    exp.configure_monitor(100ms, 16);
    ::bench::ConfigFactory f;

    {
        f.configure_thread_nr({18, 24, 32});
        f.add_option<size_t>("read_rate", {100, 50, 0});
    }

    exp.launch(f.generate_configs());

    LOG(INFO) << "PASS.";
}