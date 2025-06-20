#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <queue>
#include <set>

#include "Common.h"
#include "PerThread.h"
#include "Rdma.h"
#include "bench/DataFrame.h"
#include "bench/experiment_impl.h"
#include "bench/manager.h"
#include "boost/thread/barrier.hpp"
#include "dsm_experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "sherman/Tree.h"
#include "util/DataFrameF.h"
#include "util/PerformanceReporter.h"
#include "util/Rand.h"
#include "util/TimeConv.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"

DEFINE_double(z, 0.99, "The skewness.");
DEFINE_uint32(read_ratio, 100, "The read ratio [0, 100]");

using namespace util::literals;
using namespace std::chrono_literals;
using namespace sherman;
using Tree = sherman::Tree;

using IBenchConfig = ::bench::IBenchConfig;

uint64_t kKeySpace = 16_MB;

inline Key to_key(uint64_t k)
{
    return (CityHash64((char *) &k, sizeof(k)) + 1) % kKeySpace;
}

class RequsetGenBench : public RequstGen
{
public:
    RequsetGenBench(int coro_id, DSM *dsm, int id)
        : coro_id(coro_id), dsm(dsm), id(id)
    {
        seed = util::rdtsc();
        mehcached_zipf_init(&state,
                            kKeySpace,
                            FLAGS_z,
                            (util::rdtsc() & (0x0000ffffffffffffull)) ^ id);
    }

    Request next() override
    {
        Request r;
        uint64_t dis = mehcached_zipf_next(&state);

        r.k = to_key(dis);
        r.v = 23;
        r.is_search = rand_r(&seed) % 100 < FLAGS_read_ratio;

        return r;
    }

private:
    [[maybe_unused]] int coro_id;
    [[maybe_unused]] DSM *dsm;
    [[maybe_unused]] int id;

    unsigned int seed;
    struct zipf_gen_state state;
};

RequstGen *coro_func(int coro_id, DSM *dsm, int id)
{
    return new RequsetGenBench(coro_id, dsm, id);
}

struct BenchConfig : public ::bench::IBenchConfig
{
    using Pointer = std::shared_ptr<BenchConfig>;
    size_t thread_nr_;
    size_t coro_nr_;

    size_t thread_nr() const override
    {
        return thread_nr_;
    }
    size_t coro_nr() const override
    {
        return coro_nr_;
    }

    static BenchConfig::Pointer get_conf(size_t thread_nr, size_t coro_nr)
    {
        auto ret = std::make_shared<BenchConfig>();
        ret->thread_nr_ = thread_nr;
        ret->coro_nr_ = coro_nr;
        return ret;
    }
    bool report_latency() const
    {
        return thread_nr_ == kMaxAppThread && coro_nr_ == 1;
    }
};

std::ostream &operator<<(std::ostream &os, const BenchConfig &c)
{
    os << "{Config name: " << c.name() << ", thread: " << c.thread_nr()
       << ", coro: " << c.coro_nr() << "}";
    return os;
}

using Config = BenchConfig;
struct Spec
{
};

class Experiment : public ::bench::DSMExperiment<Spec, Config>
{
public:
    using Base = ::bench::DSMExperiment<Spec, Config>;
    using BLS = typename Base::BLS;
    Experiment()
    {
        DSMConfig config;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();
        tree_ = Tree::new_instance(
            dsm_, dsm_->alloc(4_KB), TreeConfig{}, dsm_->getClusterSize() - 1);
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
        CHECK_EQ(tid, util::get_thread_id());

        uint64_t uuid = dsm_->get_node_id() * config.thread_nr() + tid;

        tree_->run_coroutine_lock_bench(
            coro_func, uuid, config.coro_nr(), is_master, token);
    }
    void on_end_bench(const ::bench::ResultRecord &result,
                      BLS &bls,
                      const Config &conf) override
    {
        df.reg_result(result, conf);
        if (conf.report_latency())
        {
            df.reg_latency(result, conf);
        }
        Base::on_end_bench(result, bls, conf);
    }

    void exit() override
    {
        df.dump(FLAGS_binary, FLAGS_exec_meta);
        Base::exit();
    }

private:
    DSM::pointer dsm_;
    Tree::pointer tree_;

    std::unique_ptr<OnePassBucketMonitor<uint64_t>> lat_m;
    bench::ResultDataFrame df;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    std::vector<std::shared_ptr<IBenchConfig>> bench_configs;
    // for (size_t thread_nr : {1, 4, 8, 12, 16, 18, 20, 24, 32})
    for (size_t thread_nr : {1, 4, 8, 12, 16, 20})
    {
        CHECK_LE(thread_nr, kMaxAppThread);
        for (size_t coro_nr : {1, 3})
        {
            auto config = BenchConfig::get_conf(thread_nr, coro_nr);
            bench_configs.push_back(config);
        }
    }

    Experiment exp;
    exp.launch(bench_configs);
}
