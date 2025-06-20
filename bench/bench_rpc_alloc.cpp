#include <chrono>
#include <thread>

#include "Common.h"
#include "GlobalAddress.h"
#include "Metrics.h"
#include "PerThread.h"
#include "Timer.h"
#include "avis/avis.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/PerformanceReporter.h"
#include "util/gflags_dec.h"
#include "util/gflags_def.h"

struct Spec
{
};

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using BLS = typename Base::BLS;

    using Provider = avis::BuddyProvider;
    Experiment()
    {
        DSMConfig config;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();

        server_nid_ = dsm_->getClusterSize() - 1;
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }
    bool is_server() const
    {
        return dsm_->get_node_id() == server_nid_;
    }
    bool is_client() const
    {
        return !is_server();
    }
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        Base::on_start_bench(bls, conf);
    }
    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        auto &id = bls.id();
        bool is_master = id.is_master_thread() && id.is_master_worker_coro();

        auto size = conf.get<size_t>("size");

        auto min = std::chrono::nanoseconds(0ns).count();
        auto max = std::chrono::nanoseconds(1us).count();
        auto step = std::chrono::nanoseconds(1ns).count();
        OnePassBucketMonitor<uint64_t> lat_m1(min, max, step);
        min = std::chrono::nanoseconds(1ms).count();
        max = std::chrono::nanoseconds(10ms).count();
        step = std::chrono::nanoseconds(100ns).count();
        OnePassBucketMonitor<uint64_t> lat_m2(min, max, step);

        ChronoTimer timer(is_master /* enable */);
        LOG(WARNING) << "[bench] calling dsm->rpc_alloc(...) without coroutine "
                     << &ctx;

        while (!token->stop_requested())
        {
            if (is_client())
            {
                timer.pin();
                // auto raddr = handle->alloc(size);
                auto raddr = dsm_->rpc_alloc_from(size, server_nid_, nullptr);
                auto ns = timer.pin();
                if (is_master)
                {
                    lat_m1.collect(ns);
                    lat_m2.collect(ns);
                }
                if (!raddr.is_null())
                {
                    // raddrs.push_back(raddr);
                }
                else
                {
                    LOG(WARNING) << "Run out of memory";
                    token->core().request_stop();
                }
                token->complete_task(1);
                if (is_master)
                {
                    token->collect_ns(ns);
                }
            }
        }

        if (is_master)
        {
            auto p50 = lat_m2.percentile(0.50);
            auto p90 = lat_m2.percentile(0.90);
            auto p99 = lat_m2.percentile(0.99);
            auto p999 = lat_m2.percentile(0.999);
            auto p9999 = lat_m2.percentile(0.9999);

            ResultRecord result;

            result.lat_ns.p50 = p50;
            result.lat_ns.p90 = p90;
            result.lat_ns.p99 = p99;
            result.lat_ns.p999 = p999;
            result.lat_ns.p9999 = p9999;

            df().reg_latency(result, conf);

            LOG_IF(INFO, is_master) << PRE(lat_m1);
            LOG_IF(INFO, is_master) << PRE(lat_m2);
            // for (double p : {0.99, 0.999, 0.9999, 0.99999})
            // {
            //     auto test = lat_m2.percentile(p);
            //     LOG_IF(INFO, test) << "[bench] p: " << p << " got " << *test;
            // }
        }
    }
    void on_end_bench(const ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        Base::on_end_bench(res, bls, conf);
    }

private:
    DSM::pointer dsm_;
    std::vector<Provider> providers_;

    GlobalAddress pub_meta_;

    bench::ResultDataFrame lat_df_;

    size_t server_nid_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    Experiment exp;
    exp.configure_monitor(10ms, 10);

    ::bench::ConfigFactory f;
    // f.configure_thread_nr({1, 2, 4, 8, 16, 24, kMaxAppThread});
    // f.configure_coro_nr({1, 3, 5});
    f.configure_thread_nr({24});
    f.configure_coro_nr({3});
    f.add_option<size_t>("size", {64});

    exp.launch_coroutines(f.generate_configs());

    LOG(INFO) << "PASS.";
}