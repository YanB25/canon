#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "DSMCache.h"
#include "GlobalAddress.h"
#include "WRLock.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
#include "util/gflags_def.h"

struct ThreadContext
{
    std::vector<std::pair<void *, size_t>> set;
    OnePassBucketMonitor<uint64_t> bucket{0, MAX_MACHINE, 1};
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
                   bool) override
    {
        auto &tlc = bls.thread();

        size_t nr = conf.get<size_t>("alloc_nr");
        size_t alloc_size = conf.get<size_t>("alloc_size");
        for (size_t i = 0; i < nr; ++i)
        {
            auto gaddr = alloc.current()->alloc(alloc_size);
            tlc.set.emplace_back((void *) gaddr.val, alloc_size);
            tlc.bucket.collect(gaddr.nodeID);
        }

        token->core().request_stop();
    }
    void on_thread_start_bench(BLS &bls, const Config &conf) override
    {
        alloc.current() = std::make_shared<memory::DSMAllocator>(dsm_.get());
        Base::on_thread_start_bench(bls, conf);
    }
    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        auto &tlc = bls.thread();
        // validation
        for (size_t i = 0; i < tlc.set.size(); ++i)
        {
            for (size_t j = i + 1; j < tlc.set.size(); ++j)
            {
                Buffer buf_1((char *) tlc.set[i].first, tlc.set[i].second);
                Buffer buf_2((char *) tlc.set[j].first, tlc.set[j].second);
                validate_buffer_not_overlapped(buf_1, buf_2);
            }
        }

        // combine
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto &[addr, size] : tlc.set)
            {
                all_set.emplace_back(addr, size);
            }
        }

        for (const auto &[addr, size] : tlc.set)
        {
            auto gaddr = GlobalAddress(addr);
            alloc.current()->free(gaddr, size);
        }

        Base::on_thread_end_bench(bls, conf);
    }
    void on_end_bench(const ::bench::ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        // validate not overlapped
        for (size_t i = 0; i < all_set.size(); ++i)
        {
            for (size_t j = i + 1; j < all_set.size(); ++j)
            {
                auto [m1, s1] = all_set[i];
                auto [m2, s2] = all_set[j];
                validate_buffer_not_overlapped(Buffer((char *) m1, s1),
                                               Buffer((char *) m2, s2));
            }
        }
        LOG(INFO) << "validate size: " << all_set.size();
        Base::on_end_bench(res, bls, conf);
    }

private:
    DSM::pointer dsm_;
    Perthread<std::shared_ptr<memory::DSMAllocator>> alloc;

    mem::BlockAllocator::pointer g_alloc;
    std::mutex mu_;
    std::vector<std::pair<void *, size_t>> all_set;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    f.configure_thread_nr({kMaxAppThread});
    f.add_option<size_t>("alloc_nr", {500});
    f.add_option<size_t>("alloc_size", {1_KB, 1_KB, 8_B, 8_B});
    auto configs = f.generate_configs();

    exp.configure_monitor(100ms, 30);
    exp.launch(configs);

    LOG(INFO) << "PASS.";
}