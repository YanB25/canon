#include <numa.h>

#include <thread>

#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/DSMBackend.h"
#include "util/ThreadSafeHashCache.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"

using K = GlobalAddress;  // actually GlobalAddress
using V = util::Page;
constexpr static size_t kBucketNr = 1;
constexpr static size_t kCacheSize = 10;

DEFINE_uint32(page_nr, 20, "The number of pages used.");

constexpr static size_t kLeaderNodeId = 0;

struct Record
{
    const char *act;
    K key;
    int value;
};

struct ThreadContext
{
    std::map<GlobalAddress, char> value;
};

struct Spec
{
    using TLS = ThreadContext;
};

class Experiment : public ::bench::DSMExperiment<Spec>
{
public:
    using Base = ::bench::DSMExperiment<Spec>;
    using BLS = typename Base::BLS;
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
    void thread_init(BLS &bls, size_t thread_nr) override
    {
        get_dsm()->registerThread();

        for (size_t i = 0; i < FLAGS_page_nr; ++i)
        {
            gaddrs_.current().push_back(dsm_->alloc(kInternalPageSize));
        }
        Base::thread_init(bls, thread_nr);
    }

    GlobalAddress gen_key(TLS &)
    {
        auto idx = fast_pseudo_rand_int(0, gaddrs_.current().size() - 1);
        return gaddrs_.current()[idx];
    }
    Page gen_value(TLS &tlc, GlobalAddress key)
    {
        Page page(dsm_.get(), kInternalPageSize);
        auto &val = tlc.value[key];
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

    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        auto &tlc = bls.thread();

        auto nid = dsm_->get_node_id();

        std::vector<GlobalAddress> gaddrs;
        if (nid != kLeaderNodeId)
        {
            return;
        }

        auto read_rate = conf.get<size_t>("read_rate");
        auto evict_rate = conf.get<size_t>("evict_rate");
        CHECK_LE(read_rate, 100);
        CHECK_LE(evict_rate, 100);
        while (!token->stop_requested())
        {
            int action = choose_action(read_rate, evict_rate);
            auto self_key = gen_key(tlc);
            if (action == 0)
            {
                // read
                auto value = cache_->get(self_key, true);
                const auto &page = *CHECK_NOTNULL(value);
                char ch = (page.data())[0];

                his_.current().add(
                    Record{.act = "get", .key = self_key, .value = ch});

                if (tlc.value.count(self_key))
                {
                    auto expect_ch = tlc.value[self_key];
                    CHECK_EQ((int) page.data()[0], (int) expect_ch)
                        << ": " << PRE(tlc.value) << " key is " << PRE(self_key)
                        << " tid: " << util::get_thread_id();
                }
                else
                {
                    // fill that value
                    tlc.value[self_key] = page.data()[0];
                    his_.current().add(Record{.act = "fetch-on-get",
                                              .key = self_key,
                                              .value = tlc.value[self_key]});
                }
            }
            else if (action == 1)
            {
                // insert
                auto expect_value = tlc.value[self_key];
                auto value = gen_value(tlc, self_key);
                his_.current().add(Record{
                    .act = "put", .key = self_key, .value = value.data()[0]});
                auto ins = cache_->put(self_key, std::move(value));

                if (ins)
                {
                    // overwrite
                    CHECK(tlc.value.count(self_key));
                    auto &page = *ins;
                    CHECK_EQ(page.data()[0], expect_value);
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
                auto old_entry = cache_->invalidate(self_key);
                if (old_entry)
                {
                    auto &page = *old_entry;
                    if (tlc.value.count(self_key))
                    {
                        CHECK_EQ(page.data()[0], tlc.value[self_key]);
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
            cache_->drop_all();
        }
        Base::exit();
    }

private:
    DSM::pointer dsm_;
    std::unique_ptr<CacheT> cache_;
    Perthread<util::TimedHistory<Record>> his_;
    Perthread<std::vector<GlobalAddress>> gaddrs_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    ::bench::ConfigFactory f;

    // trivial
    {
        f.configure_thread_nr({1, 4, 18, kMaxAppThread});
        f.add_option<size_t>("read_rate", {100, 50, 0});
        f.add_option<size_t>("evict_rate", {50, 0});
    }

    // {
    //     f.configure_thread_nr({2});
    //     f.add_option<size_t>("page_nr", {10});
    //     f.add_option<size_t>("read_rate", {100});
    //     f.add_option<size_t>("evict_rate", {0});
    // }

    Experiment exp;
    exp.configure_monitor(500ms, 10);
    exp.launch(f.generate_configs());

    LOG(INFO) << "PASS.";
}