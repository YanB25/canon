#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "Common.h"
#include "DSMCache.h"
#include "WRLock.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
#include "util/gflags_def.h"

struct ThreadContext
{
    std::queue<std::pair<void *, size_t>> set;
    OnePassBucketMonitor<uint64_t> bucket{0, MAX_MACHINE, 1};
    OnePassBucketMonitor<uint64_t> lat{
        0, util::time::to_ns(1us), util::time::to_ns(1ns)};
};
struct Spec
{
    using TLS = ThreadContext;
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    using BLS = typename Base::BLS;
    Experiment()
    {
        DSMConfig config;
        config.worker_nr = 0;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();
    }
    void thread_init(BLS &bls, size_t thread_nr) override
    {
        dsm_->registerThread();
        Base::thread_init(bls, thread_nr);
    }
    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool is_master) override
    {
        auto &tlc = bls.thread();
        size_t alloc_size = conf.get<size_t>("alloc_size");
        auto free_rate = conf.get<size_t>("free_rate");
        bool with_read = conf.get<bool>("with_read");
        ChronoTimer timer;
        while (!token->stop_requested())
        {
            if (is_master)
            {
                timer.pin();
            }
            bool should_free =
                fast_pseudo_bool_with_prob(1.0 * free_rate / 100);
            bool should_alloc = !should_free;
            if (tlc.set.empty() || should_alloc)
            {
                // alloc
                auto gaddr = dsm_->alloc(alloc_size);

                if (likely(!gaddr.is_null()))
                {
                    token->complete_task();
                    tlc.set.emplace((void *) gaddr.val, alloc_size);
                    if (with_read)
                    {
                        auto rdma_buf = dsm_->get_rdma_buffer(alloc_size);
                        dsm_->prepare_read(
                            rdma_buf.buffer, gaddr, alloc_size, false, nullptr);
                        dsm_->commit(nullptr);
                        dsm_->put_rdma_buffer(std::move(rdma_buf));
                    }
                }
                else
                {
                    LOG_FIRST_N(WARNING, 1)
                        << "Run out of memory: " << dsm_->dsm_usage();
                }
            }
            else
            {
                // free
                auto [addr, size] = tlc.set.front();
                DCHECK(!tlc.set.empty());
                tlc.set.pop();
                GlobalAddress gaddr(addr);

                dsm_->free(gaddr, size);
                token->complete_task();
            }
            if (is_master)
            {
                auto ns = timer.pin();
                tlc.lat.collect(ns);
            }
        }

        LOG_IF(INFO, is_master) << tlc.lat;
    }

private:
    DSM::pointer dsm_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    f.configure_thread_nr({kMaxAppThread});
    f.add_option<size_t>("free_rate", {0});
    // f.add_option<size_t>("alloc_size", {1_KB, 1_KB, 8_B, 8_B});
    f.add_option<size_t>("alloc_size", {1_MB});
    f.add_option<bool>("with_read", {false});
    auto configs = f.generate_configs();

    exp.configure_monitor(100ms, 10);
    exp.launch(configs);

    LOG(INFO) << "PASS.";
}