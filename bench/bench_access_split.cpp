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
#include "bench/experiment.h"
#include "boost/thread/barrier.hpp"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "patronus/All.h"
#include "patronus/patronus_experiment.h"
#include "util/DataFrameF.h"
#include "util/PerformanceReporter.h"
#include "util/Rand.h"
#include "util/TimeConv.h"
#include "util/gflags_def.h"

using namespace util::literals;
using namespace patronus;
using namespace std::chrono_literals;
using namespace ::bench;

using IBenchConfig = ::bench::IBenchConfig;

constexpr static size_t kServerThreadNr = 1;

struct BenchConfig : public ::bench::IBenchConfig
{
    using Pointer = std::shared_ptr<BenchConfig>;
    size_t thread_nr_;
    size_t coro_nr_;
    size_t block_size;
    size_t split_nr;

    size_t thread_nr() const override
    {
        return thread_nr_;
    }
    size_t coro_nr() const override
    {
        return coro_nr_;
    }

    std::string name() const override
    {
        return "sz(" + std::to_string(block_size) + ")split(" +
               std::to_string(split_nr) + ")";
    }

    static BenchConfig::Pointer get_conf(size_t thread_nr,
                                         size_t coro_nr,
                                         size_t block_size,
                                         size_t split_nr)
    {
        auto ret = std::make_shared<BenchConfig>();
        ret->thread_nr_ = thread_nr;
        ret->coro_nr_ = coro_nr;
        ret->block_size = block_size;
        ret->split_nr = split_nr;
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
       << ", coro: " << c.coro_nr() << ", block size: " << c.block_size
       << ", split: " << c.split_nr << "}";
    return os;
}

using Config = BenchConfig;

struct Spec
{
};

class Benchmark : public ::bench::IPatronusExperiment<Spec, Config>
{
public:
    using Base = ::bench::IPatronusExperiment<Spec, Config>;
    using BLS = typename Base::BLS;
    void init() override
    {
        PatronusConfig config;

        config.disable_mw = true;
        config.simplify = true;
        config.client_rdma_buffer = {{256_B, 4_KB}, {0.1, 0.9}};
        p_ = Patronus::ins(config);

        Base::init();
    }
    Patronus::pointer get_patronus() const override
    {
        return p_;
    }
    void patronus_worker(Patronus::pointer p,
                         BLS &bls,
                         const Config &conf,
                         CoroContext &ctx,
                         ::bench::StopToken::pointer finished,
                         bool is_master) override
    {
        std::ignore = bls;
        auto tid = p->get_thread_id();
        auto coro_id = ctx.coro_id();

        bool unique_master = is_master && coro_id == 0;

        auto dir_id = tid % kServerThreadNr;
        [[maybe_unused]] auto nid = p->get_node_id();

        auto server_list = ::config::get_server_nids();
        auto server_nid = server_list[nid % server_list.size()];

        ChronoTimer timer;
        size_t actual_run = 0;

        size_t io_nr = conf.split_nr;
        CHECK_GT(io_nr, 0);
        size_t io_size = conf.block_size / io_nr;
        CHECK_GT(io_size, 0);

        auto dsm = p->get_dsm();
        auto max_offset = dsm->buffer_size() - 2 * conf.block_size;

        ChronoTimer op_timer;
        PatronusBatchContext<32> batch;
        while (true)
        {
            if (unique_master)
            {
                op_timer.pin();
            }
            std::vector<Buffer> rdma_buffers;
            for (size_t i = 0; i < io_nr; ++i)
            {
                GlobalAddress gaddr;
                auto rand_offset =
                    64 * fast_pseudo_rand_int(0, max_offset / 64);
                rdma_buffers.emplace_back(p->get_rdma_buffer(io_size));
                p->prepare_unprot_read(
                     batch,
                     server_nid,
                     dir_id,
                     CHECK_NOTNULL(rdma_buffers.back().buffer),
                     io_size,
                     rand_offset,
                     &ctx)
                    .expect(RC::kOk);
            }
            p->commit(batch, &ctx).expect(RC::kOk);

            for (auto &&buf : rdma_buffers)
            {
                p->put_rdma_buffer(std::move(buf));
            }
            rdma_buffers.clear();

            // if (unique_master)
            // {
            //     auto op_ns = op_timer.pin();
            // }

            actual_run++;
            bool has_more_task = finished->complete_task(1);
            if (!has_more_task)
            {
                break;
            }
        }

        auto total_ns = timer.pin();

        VLOG(V) << "[bench] tid " << tid << " finished op " << actual_run
                << " within " << total_ns << " ns. coro: " << ctx;
    }
    void on_start_bench(BLS &bls, const Config &config) override
    {
        timer.pin();
        actual_run = 0;
        auto min = util::time::to_ns(0ns);
        auto max = util::time::to_ns(10ms);
        auto range = util::time::to_ns(1us);
        lat_m =
            std::make_unique<OnePassBucketMonitor<uint64_t>>(min, max, range);
        LOG(INFO) << "[bench] start bench: " << config;
        Base::on_start_bench(bls, config);
    }
    void on_end_bench(const ResultRecord &results,
                      BLS &bls,
                      const Config &conf) override
    {
        LOG(INFO) << "[result] " << results;
        df.reg_result(results, conf);
        col_x_alloc_size.push_back(conf.block_size);
        col_x_split_nr.push_back(conf.split_nr);

        if (conf.report_latency())
        {
            df.reg_latency(results, conf);
        }
        LOG(INFO) << "[bench] latency: " << results.lat_ns;
        Base::on_end_bench(results, bls, conf);
    }
    void exit() override
    {
        df.load_column("split_nr", std::move(col_x_split_nr));
        df.dump(FLAGS_binary.c_str(), FLAGS_exec_meta);
        Base::exit();
    }

private:
    Patronus::pointer p_;

    std::unique_ptr<OnePassBucketMonitor<uint64_t>> lat_m;
    ChronoTimer timer;
    std::atomic<int64_t> actual_run;

    std::vector<uint64_t> col_x_alloc_size;
    std::vector<uint64_t> col_x_split_nr;
    ::bench::ResultDataFrame df;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    auto nid = std::stoi(FLAGS_node_id);
    bool is_client = ::config::is_client(nid);

    std::vector<std::shared_ptr<IBenchConfig>> bench_configs;
    for (size_t thread_nr : {kMaxAppThread})
    {
        CHECK_LE(thread_nr, kMaxAppThread);
        for (size_t block_size : {256_B, 512_B, 1024_B, 2048_B, 4_KB})
        // for (size_t block_size : {64_B, 4_KB})
        {
            // for (size_t coro_nr : {1, 8, 16})
            // for (size_t coro_nr : {16})
            for (size_t coro_nr : {32})
            {
                for (size_t split_nr : {1})
                {
                    auto config = BenchConfig::get_conf(
                        thread_nr, coro_nr, block_size, split_nr);
                    bench_configs.push_back(config);
                }
            }
        }
    }

    if (is_client)
    {
        Benchmark expr;
        expr.launch_coroutines(bench_configs);
    }
    else
    {
        LOG(FATAL) << "TODO: set correct server thread number for server";
        Benchmark expr;
        expr.launch(bench_configs);
    }
}
