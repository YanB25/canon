#include <numa.h>

#include <thread>

#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/DSMBackend.h"
#include "util/History.h"
#include "util/ThreadSafeHashCache.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"

using K = GlobalAddress;  // actually GlobalAddress
using V = util::Page;
constexpr static size_t kBucketNr = 1;
constexpr static size_t kCacheSize = 4;

DEFINE_uint32(page_nr, 20, "The number of pages used.");

constexpr static size_t kLeaderNodeId = 0;

struct Record
{
    const char *act;
    K key;
    int value;
};

struct Spec
{
    struct CLS
    {
        std::map<GlobalAddress, char> value;
    };
};
class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using CacheT = util::hash::ThreadSafeHashCache<K, V>;
    using BackendT = util::DSMBackend;

    using Page = util::Page;

    Experiment()
    {
        DSMConfig config;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();

        cache_ = std::make_unique<CacheT>(
            BackendT::make_ptr(dsm_), kBucketNr, kCacheSize);
    }

    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }
    auto &gaddrs(coro_t coro_id)
    {
        return gaddrs_.current()[coro_id];
    }
    void thread_init(BLS &bls, size_t thread_nr) override
    {
        get_dsm()->registerThread();

        for (size_t cid = 0; cid < define::kMaxCoroNr; ++cid)
        {
            for (size_t i = 0; i < FLAGS_page_nr; ++i)
            {
                gaddrs(cid).push_back(dsm_->alloc(kInternalPageSize));
            }
        }
        Base::thread_init(bls, thread_nr);
    }

    GlobalAddress gen_key(CoroContext &ctx)
    {
        auto &self_gaddrs = gaddrs(ctx.coro_id());
        auto idx = fast_pseudo_rand_int(0, self_gaddrs.size() - 1);
        return self_gaddrs[idx];
    }

    Page gen_value(CLS &cls, GlobalAddress key)
    {
        Page page(dsm_.get(), kInternalPageSize);
        auto &val = cls.value[key];
        val++;
        page.data()[0] = val;
        return page;
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

    void on_start_bench(BLS &bls, const Config &conf) override
    {
        cache_ = std::make_unique<CacheT>(
            BackendT::make_ptr(dsm_), kBucketNr, kCacheSize);

        Base::on_start_bench(bls, conf);
    }

    void expect_eq(char expect, char actual, const K &key)
    {
        if (unlikely(expect != actual))
        {
            // let all other clients exit
            // token->core().request_stop();
            // std::this_thread::sleep_for(10ms);

            using namespace ranges;

            auto filter =
                views::filter([&key](const auto &e) { return e->key == key; });
            auto ret = his_.take(10, filter);

            LOG(WARNING) << "history: " << PRE(ret);
            auto filter2 = views::filter([&key](const auto &r)
                                         { return r.t().key == key; });

            const auto &cache_tl_history = cache_->tl_history();
            auto cache_history = cache_tl_history.take(10, filter2);

            LOG(WARNING) << "cache history: " << PRE(cache_history);

            CHECK_EQ(expect, actual) << "** mismatch for " << PRE(key)
                                     << ". tid: " << util::get_thread_id();
            return;
        }
    }

    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        auto &cls = bls.coroutine();

        auto nid = dsm_->get_node_id();

        std::vector<GlobalAddress> gaddrs;
        if (nid != kLeaderNodeId)
        {
        }

        auto read_rate = conf.get<size_t>("read_rate");
        auto evict_rate = conf.get<size_t>("evict_rate");
        CHECK_LE(read_rate, 100);
        CHECK_LE(evict_rate, 100);
        while (!token->stop_requested())
        {
            int action = choose_action(read_rate, evict_rate);
            auto self_key = gen_key(ctx);
            if (action == 0)
            {
                // read
                auto value = cache_->get(self_key, true, &ctx);
                const auto &page = *CHECK_NOTNULL(value);
                char ch = (page.data())[0];

                his_.current().add(
                    Record{.act = "get", .key = self_key, .value = ch});

                if (cls.value.count(self_key))
                {
                    auto expect_ch = cls.value[self_key];
                    expect_eq(expect_ch, page.data()[0], self_key);
                }
                else
                {
                    // fill that value
                    cls.value[self_key] = page.data()[0];
                    his_.current().add(Record{.act = "fetch-on-get",
                                              .key = self_key,
                                              .value = cls.value[self_key]});
                }
            }
            else if (action == 1)
            {
                // insert
                auto expect_value = cls.value[self_key];
                auto value = gen_value(cls, self_key);
                his_.current().add(Record{
                    .act = "put", .key = self_key, .value = value.data()[0]});
                auto ins = cache_->put(self_key, std::move(value), &ctx);

                if (ins)
                {
                    // overwrite
                    CHECK(cls.value.count(self_key));
                    auto &page = *ins;
                    expect_eq(expect_value, page.data()[0], self_key);
                }
                else
                {
                    // insert
                }
            }
            else
            {
                CHECK_EQ(action, 2);
                // invalidate
                auto old_entry = cache_->invalidate(self_key, &ctx);
                if (old_entry)
                {
                    auto &page = *old_entry;
                    if (cls.value.count(self_key))
                    {
                        expect_eq(
                            cls.value[self_key], page.data()[0], self_key);
                    }
                    his_.current().add(Record{.act = "inv",
                                              .key = self_key,
                                              .value = page.data()[0]});
                }
                else
                {
                }
            }

            token->complete_task(1);
        }
    }
    void exit() override
    {
        if (cache_)
        {
            // do not write to backend
            cache_->drop_all();
        }
        Base::exit();
    }

private:
    DSM::pointer dsm_;
    std::unique_ptr<CacheT> cache_;
    util::TL_History<Record> his_;
    Perthread<std::array<std::vector<GlobalAddress>, define::kMaxCoroNr>>
        gaddrs_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    ::bench::ConfigFactory f;

    {
        f.configure_thread_nr({1, 4, 18, kMaxAppThread});
        f.configure_coro_nr({1, 3});
        f.add_option<size_t>("read_rate", {100, 50, 0});
        f.add_option<size_t>("evict_rate", {50, 0});
    }

    // {
    //     f.configure_thread_nr({1});
    //     f.configure_coro_nr({3});
    //     f.add_option<size_t>("page_nr", {10});
    //     f.add_option<size_t>("read_rate", {50});
    //     f.add_option<size_t>("evict_rate", {0});
    // }

    Experiment exp;
    exp.configure_monitor(200ms, 20);
    exp.launch_coroutines(f.generate_configs());

    LOG(INFO) << "PASS.";
}