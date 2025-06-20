#include <numa.h>

#include <thread>

#include "bench/DataFrame.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/DSMBackend.h"
#include "util/ThreadSafeHashCache.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"

using K = GlobalAddress;  // actually GlobalAddress
using V = util::Page;

DEFINE_uint32(cache_bucket_nr, 2150, "The number of bucket");
DEFINE_uint32(cache_size, 2150, "The size of cache");

DEFINE_uint32(page_nr, 20, "The number of pages used.");
DEFINE_uint32(max_page_nr, 16_M * 8 / 1024, "The number of max page");

DEFINE_double(z, 0.99, "zipfian parameter");

struct Spec
{
    struct TLS
    {
        util::ZipfianGenerator g{
            0, FLAGS_max_page_nr - 1, FLAGS_z, util::get_thread_id()};
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
            BackendT::make_ptr(dsm_), FLAGS_cache_bucket_nr, FLAGS_cache_size);
    }

    void init() override
    {
        if (!dsm_->hasRegistered())
        {
            dsm_->registerThread();
        }
        pages_.reserve(FLAGS_max_page_nr);
        for (size_t i = 0; i < FLAGS_max_page_nr; ++i)
        {
            pages_.emplace_back(dsm_->alloc(kInternalPageSize));
        }
        LOG(INFO) << "Init: with page " << util::pre_num(FLAGS_max_page_nr);
        Base::init();
    }

    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }
    GlobalAddress gen_key(TLS &tls)
    {
        auto next_page_id = tls.g.Next();
        return pages_[next_page_id];
    }

    void on_start_bench(BLS &bls, const Config &conf) override
    {
        cache_ = std::make_unique<CacheT>(
            BackendT::make_ptr(dsm_), FLAGS_cache_bucket_nr, FLAGS_cache_size);

        Base::on_start_bench(bls, conf);
    }

    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        std::ignore = conf;
        auto &tls = bls.thread();

        auto read_ratio = conf.get<uint64_t>("read_ratio");

        util::Page self_page(dsm_.get(), kInternalPageSize);

        while (!token->stop_requested())
        {
            const auto &key = gen_key(tls);

            bool is_read = fast_pseudo_bool_with_prob(read_ratio / 100.0);
            if (is_read)
            {
                auto page = cache_->get(key, true, &ctx);
                self_page.from(page->data(), kInternalPageSize);
            }
            else
            {
                auto new_page = self_page;
                cache_->put(key, std::move(new_page), &ctx);
            }
            token->complete_task(1);
        }
    }
    void on_end_bench(const ::bench::ResultRecord &results,
                      BLS &s,
                      const Config &conf) override
    {
        df.reg_result(results, conf);

        auto &metrics = cache_->metrics();

        auto acc = metrics.accumulate(
            [](auto acc, const auto &cur) { return acc + cur; },
            util::hash::CacheMetric{});

        LOG(INFO) << PRE(acc);

        df.reg_result("io_rate", (uint64_t)(100.0 * acc.io_rate()));

        Base::on_end_bench(results, s, conf);
    }
    void exit() override
    {
        if (cache_)
        {
            // do not write to backend
            cache_->drop_all();
        }
        df.dump(FLAGS_binary.c_str(), FLAGS_exec_meta);
        Base::exit();
    }

private:
    DSM::pointer dsm_;
    std::unique_ptr<CacheT> cache_;
    std::vector<GlobalAddress> pages_;

    bench::ResultDataFrame df;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    ::bench::ConfigFactory f;

    {
        f.configure_thread_nr({8, 12, 18, 24, 32});
        // f.configure_coro_nr({1, 3});  // 3 always better than 1
        f.configure_coro_nr({3});
        f.add_option_df("read_ratio", {0, 100});
    }

    Experiment exp;
    exp.configure_monitor(400ms, 10);
    exp.launch_coroutines(f.generate_configs());

    LOG(INFO) << "PASS.";
}