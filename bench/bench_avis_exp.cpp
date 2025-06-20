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
    struct CLS
    {
        std::shared_ptr<avis::AvisHandle> handle_;
        avis::AvisAdaptor::Pointer adpt_;
    };
};

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using BLS = typename Base::BLS;

    using Provider = avis::BuddyProvider;
    Experiment(size_t buddy_nr = 1)
    {
        DSMConfig config;
        config.relaxed_ordering = false;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();

        server_nid_ = dsm_->getClusterSize() - 1;

        if (is_server())
        {
            uint32_t nid = dsm_->get_node_id();
            std::vector<Provider> providers;
            for (size_t i = 0; i < buddy_nr; ++i)
            {
                auto meta = dsm_->alloc_from(2_MB, nid, 4_KB);
                auto data = dsm_->alloc_from(2_GB, nid, 4_KB);
                providers.emplace_back(Provider{
                    .node_id = nid,
                    .meta_raddr = meta,
                    .meta_size = 2_MB,
                    .buf_raddr = data,
                    .buf_size = 2_GB,
                    .page_size = 4_KB,
                });
            }
            dsm_->put("size", (uint32_t) providers.size(), 100ms);
            dsm_->put("providers",
                      providers.data(),
                      providers.size() * sizeof(Provider),
                      100ms);

            auto pub_size = 2_MB;
            auto pub_meta = dsm_->alloc_from(pub_size, server_nid_);
            auto rdma_buf = dsm_->get_rdma_buffer(pub_size);
            memset(rdma_buf.buffer, 0, pub_size);
            dsm_->prepare_write(
                rdma_buf.buffer, pub_meta, pub_size, false, nullptr);
            dsm_->commit();
            dsm_->put_rdma_buffer(std::move(rdma_buf));
            dsm_->put("pub", pub_meta, 100ms);
            dsm_->put("pub_size", pub_size, 100ms);
        }
        auto size = dsm_->get<uint32_t>("size", 100ms);
        auto *raw = dsm_->get_raw("providers", 100ms);
        providers_.resize(size);
        memcpy(providers_.data(), raw, size * sizeof(Provider));

        pub_meta_ = dsm_->get<GlobalAddress>("pub", 100ms);
        pub_size_ = dsm_->get<size_t>("pub_size", 100ms);
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
        total_am_.reset();
        Base::on_start_bench(bls, conf);
    }
    void init_adpt(BLS &bls, CoroContext *ctx)
    {
        auto &cls = bls.coroutine();

        auto ptl = std::make_shared<avis::PTL>(dsm_, server_nid_, ctx);
        auto pub = std::make_shared<avis::Publisher<avis::BitmapPub>>(
            dsm_, pub_meta_, pub_size_, ctx);

        cls.handle_ =
            avis::AvisHandle::make_ptr(providers_, dsm_, ptl, pub, ctx);
        cls.adpt_ = avis::AvisAdaptor::make_ptr(cls.handle_, server_nid_);
    }
    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        auto &cls = bls.coroutine();
        auto &id = bls.id();
        bool is_master = id.is_master_thread() && id.is_master_worker_coro();

        init_adpt(bls, &ctx);

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

        auto &handle = cls.handle_;
        // std::vector<GlobalAddress> raddrs;
        while (!token->stop_requested())
        {
            if (is_client())
            {
                timer.pin();
                auto raddr = handle->alloc(size);
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

        auto [am, rm] = handle->metrics();
        {
            std::lock_guard<std::mutex> lk(mu_);
            total_am_ += am;
        }
    }
    void on_end_bench(const ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        LOG(INFO) << "Total: " << std::endl << total_am_;

        LOG(INFO) << "Reseting...";
        for (const auto &[gaddr, sz] : metas_)
        {
            auto buf = dsm_->get_rdma_buffer(sz);
            memset(buf.buffer, 0, sz);
            dsm_->prepare_write(buf.buffer, gaddr, sz, false, nullptr);
            dsm_->commit();
            dsm_->put_rdma_buffer(std::move(buf));
        }
        LOG(INFO) << "Done";

        Base::on_end_bench(res, bls, conf);
    }

private:
    DSM::pointer dsm_;
    std::vector<Provider> providers_;

    GlobalAddress pub_meta_;
    size_t pub_size_;

    // internal management
    std::vector<std::pair<GlobalAddress, size_t>> metas_;

    std::mutex mu_;
    AllocMetrics total_am_;

    bench::ResultDataFrame lat_df_;

    size_t server_nid_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    Experiment exp;
    exp.configure_monitor(20ms, 10);

    ::bench::ConfigFactory f;
    f.configure_thread_nr({1, 2, 4, 8, 16, 24, kMaxAppThread});
    f.configure_coro_nr({1, 3, 5});
    f.add_option<size_t>("size", {64});

    exp.launch_coroutines(f.generate_configs());

    LOG(INFO) << "PASS.";
}