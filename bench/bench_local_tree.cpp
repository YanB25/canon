#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <queue>
#include <set>

#include "PerThread.h"
#include "bench/DataFrame.h"
#include "bench/experiment.h"
#include "sherman/RequestGenerator.h"
#include "sherman/Tree.h"
#include "util/DSMBackend.h"
#include "util/DataFrameF.h"
#include "util/Page.h"
#include "util/PerformanceReporter.h"
#include "util/Rand.h"
#include "util/ThreadSafeHashCache.h"
#include "util/gflags_def.h"

DEFINE_double(z, 0.99, "The skewness.");
DEFINE_uint32(read_ratio, 0, "The read ratio [0, 100]");
DEFINE_uint32(preload_rate, 20, "The rate to preload data [0, 100]");
DEFINE_uint32(warmup_rate, 10, "The rate to warm up [0, 100]");
DEFINE_uint32(max_key,
              16_M,
              "The max key. Choose several million or even billion.");
DEFINE_uint64(init_cache_size, 2150, "Number of pages allowed to cache.");
DEFINE_uint64(init_cache_bucket_nr, 2150, "Number of bucket (concurrency).");

using namespace util::literals;
using namespace std::chrono_literals;
using namespace sherman;

using IBenchConfig = ::bench::IBenchConfig;
using Tree = sherman::Tree;

constexpr static size_t V = ::config::verbose::kBenchReserve_1;

using namespace hmdf;

RequstGen *coro_func(int coro_id, DSM *dsm, int id)
{
    return new sherman::RequestGenBench(
        coro_id, dsm, id, FLAGS_max_key, {{8, 1}}, FLAGS_z, FLAGS_read_ratio);
}

class Experiment : public ::bench::DSMExperiment<Void>
{
public:
    using Base = ::bench::DSMExperiment<Void>;
    using BLS = ::bench::Storage<Void>;
    using BackendT = util::DSMBackend;

    Experiment()
    {
        DSMConfig config;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();

        tree_ = Tree::new_instance(
            CHECK_NOTNULL(dsm_),
            dsm_->alloc(4_KB),
            TreeConfig{.is_local = true,
                       .bucket_nr = FLAGS_init_cache_bucket_nr,
                       .cache_limit = FLAGS_init_cache_size},
            dsm_->getClusterSize() - 1);
    }

    void init() override
    {
        // globally preload single-thread
        // controled by FLAGS_preload_rate
        if (!dsm_->hasRegistered())
        {
            dsm_->registerThread();
        }
        if (dsm_->get_node_id() == 0)
        {
            size_t preload_nr;
            // avoid handling float number
            if (FLAGS_preload_rate == 100)
            {
                preload_nr = FLAGS_max_key;
            }
            else
            {
                preload_nr = FLAGS_max_key * FLAGS_preload_rate / 100.0;
            }
            LOG(INFO) << "[master] Initial loading "
                      << util::pre_num(preload_nr) << " into the tree... ("
                      << util::pre_pcnt(FLAGS_preload_rate / 100.0) << ")";
            for (size_t i = 1; i < preload_nr; ++i)
            {
                tree_->insert(
                    sherman::RequestGenBench::to_key(i, FLAGS_max_key), i * 2);
            }
            LOG(INFO) << "[master] loading finished.";
        }

        Base::init();
    }
    void thread_init(BLS &bls, size_t thread_nr) override
    {
        // per-thread preload
        // controled by FLAGS_warmup_rate
        size_t tid = util::get_thread_id();
        if (!dsm_->hasRegistered())
        {
            dsm_->registerThread();
        }
        size_t preload_nr;
        // avoid handling float number
        if (FLAGS_warmup_rate == 100)
        {
            preload_nr = FLAGS_max_key;
        }
        else
        {
            preload_nr = FLAGS_max_key * FLAGS_warmup_rate / 100.0;
        }
        LOG_FIRST_N(INFO, 1)
            << "[master] Per-thread preloading " << util::pre_num(preload_nr)
            << " into the tree... ("
            << util::pre_pcnt(FLAGS_warmup_rate / 100.0) << ")";
        size_t all_thread = thread_nr * dsm_->getClusterSize();
        auto my_id = tid + dsm_->get_node_id() * thread_nr;
        for (size_t i = 1; i < preload_nr; ++i)
        {
            if (i % all_thread == my_id)
            {
                tree_->insert(
                    sherman::RequestGenBench::to_key(i, FLAGS_max_key), i * 2);
            }
        }
        LOG(INFO) << "[master] loading finished.";
        Base::thread_init(bls, thread_nr);
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }
    void benchmark(BLS &,
                   const Config &config,
                   ::bench::StopToken::pointer token,
                   bool is_master) override
    {
        auto tid = dsm_->get_thread_id();

        ChronoTimer timer;
        size_t actual_run = 0;

        uint64_t uuid =
            dsm_->get_node_id() * config.thread_nr() + dsm_->get_thread_id();

        tree_->run_coroutine(
            coro_func, uuid, config.coro_nr(), is_master, token);

        auto total_ns = timer.pin();

        VLOG(V) << "[bench] tid " << tid << " finished op " << actual_run
                << " within " << total_ns << " ns.";
    }
    void on_start_bench(BLS &s, const Config &config) override
    {
        LOG(INFO) << "[bench] start bench: " << config << ", " << PRE(FLAGS_z)
                  << ", " << PRE(FLAGS_read_ratio);
        auto n_item_per_bucket = config.get<uint64_t>("n_item_per_bucket");
        auto cache_size = config.get<uint64_t>("cache_size");
        auto bucket_nr = cache_size / n_item_per_bucket;
        LOG(WARNING) << "Cache with " << PRE(bucket_nr) << ", "
                     << PRE(cache_size);
        tree_->maybe_reset_page_cache(bucket_nr, cache_size);
        tree_->reset_config(TreeConfig{
            .is_local = true,
            .bucket_nr = bucket_nr,
            .cache_limit = cache_size,
        });

        Base::on_start_bench(s, config);
    }

    void on_end_bench(const ::bench::ResultRecord &results,
                      BLS &s,
                      const Config &conf) override
    {
        df.reg_result("x_read_rate", (uint64_t) (100.0 * FLAGS_read_ratio));
        df.reg_result("x_zipfian", (uint64_t) (100.0 * FLAGS_z));

        df.reg_result(results, conf);

        auto &metrics = tree_->page_cache().cache().metrics();

        auto acc = metrics.accumulate([](auto acc, const auto &cur)
                                      { return acc + cur; },
                                      util::hash::CacheMetric{});

        LOG(INFO) << PRE(acc);

        df.reg_result("io_rate", (uint64_t) (100.0 * acc.io_rate()));

        metrics.fill(util::hash::CacheMetric{});

        Base::on_end_bench(results, s, conf);
    }

    void exit() override
    {
        df.dump(FLAGS_binary.c_str(), FLAGS_exec_meta);
        Base::exit();
    }

    ~Experiment() = default;

private:
    DSM::pointer dsm_;
    Tree::pointer tree_;

    bench::ResultDataFrame df;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    CHECK_EQ(FLAGS_machine_nr, 1)
        << "This is a local benchmark: use one machine.";

    ::bench::ConfigFactory f;

    // {
    //     auto s = FLAGS_init_cache_size;
    //     f.add_option_df("cache_size",
    //                     {s * 10, s * 4, s * 2, s, s / 2, s / 4, s / 10});
    //     f.add_option_df("n_item_per_bucket", {1, 2, 4});
    //     f.configure_thread_nr({32});
    //     f.configure_coro_nr({3});
    // }

    {
        auto s = FLAGS_init_cache_size;
        f.add_option_df("cache_size", {s});
        f.add_option_df("n_item_per_bucket", {1});
        f.configure_thread_nr({8, 12, 16, 20, 24, 28, 32});
        f.configure_coro_nr({3});
    }
    // {
    //     size_t size = std::numeric_limits<ssize_t>::max();
    //     f.add_option_df("cache_size", {size});
    //     f.add_option_df("n_item_per_bucket", {size / 1024});
    //     f.configure_thread_nr({2, 8, 12, 18, 24, 32});
    //     f.configure_coro_nr({1, 3});
    // }

    Experiment exp;
    exp.configure_monitor(500ms, 10);
    exp.launch(f.generate_configs());

    LOG(INFO) << "Done.";
}
