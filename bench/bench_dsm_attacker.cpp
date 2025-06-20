#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "DSMCache.h"
#include "WRLock.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
#include "util/gflags_def.h"

struct ThreadContext
{
    GlobalAddress gaddr;
};
struct Spec
{
    using PTLS = ThreadContext;
    struct TLS
    {
        size_t op{};
    };
};

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
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
        auto &ptlc = bls.persistent_thread();
        ptlc.gaddr = dsm_->alloc(4_KB);
        LOG(INFO) << "Address is " << ptlc.gaddr;
        Base::thread_init(bls, thread_nr);
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        auto active_mr = conf.get<size_t>("irrelevant_mr");

        for (size_t i = 0; i < active_mr; ++i)
        {
            auto *huge_page = hugePageAlloc(1_MB);
            huge_pages_.push_back(huge_page);
            auto *rdma_ctx = dsm_->get_th_rdma_context(0);
            auto *mr = CHECK_NOTNULL(
                createMemoryRegion((uint64_t) huge_page, 1_MB, rdma_ctx));
            irrelevant_mr_.emplace_back(CHECK_NOTNULL(mr));
        }
        Base::on_start_bench(bls, conf);
    }
    void on_end_bench(const ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        for (size_t i = 0; i < irrelevant_mr_.size(); ++i)
        {
            auto *mr = irrelevant_mr_[i];
            auto *huge_page = huge_pages_[i];
            CHECK(destroyMemoryRegion(mr));
            CHECK(hugePageFree(huge_page, 1_MB));
        }
        irrelevant_mr_.clear();
        huge_pages_.clear();
        Base::on_end_bench(res, bls, conf);
    }

    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        // LOG(INFO) << "Tid " << util::get_thread_id() << ": " <<
        // bls.thread().op;
        Base::on_thread_end_bench(bls, conf);
    }

    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        auto &ptls = bls.persistent_thread();
        auto &tls = bls.thread();
        auto dsm = get_dsm();
        auto io_size = conf.get<size_t>("io_size");
        auto batch_size = conf.get<size_t>("batch_size");
        bool attacker = conf.get<bool>("attacker");
        [[maybe_unused]] auto thread_nr = conf.thread_nr();
        [[maybe_unused]] auto tid = util::get_thread_id();
        // auto id = ctx.coro_id() + tid * define::kMaxCoroNr;
        if (unlikely(tid == 0))
        {
            while (!token->stop_requested())
            {
                if (attacker)
                {
                    auto *huge_page = hugePageAlloc(4_KB);
                    auto *rdma_ctx = dsm->get_dir_rdma_context(0);
                    auto *mr = CHECK_NOTNULL(createMemoryRegion(
                        (uint64_t) huge_page, 4_KB, rdma_ctx));
                    CHECK(destroyMemoryRegion(mr));
                    CHECK(hugePageFree(huge_page, 4_KB));

                    for (size_t th_id = 0; th_id < thread_nr; ++th_id)
                    {
                        auto *huge_page = hugePageAlloc(4_KB);
                        auto *rdma_ctx = dsm->get_th_rdma_context(th_id);
                        auto *mr = CHECK_NOTNULL(createMemoryRegion(
                            (uint64_t) huge_page, 4_KB, rdma_ctx));
                        CHECK(destroyMemoryRegion(mr));
                        CHECK(hugePageFree(huge_page, 4_KB));
                    }
                }
            }
        }
        else
        {
            auto rdma_buffer = dsm->get_rdma_buffer(4_KB);
            while (!token->stop_requested())
            {
                for (size_t i = 0; i < batch_size; ++i)
                {
                    auto addr =
                        ptls.gaddr +
                        fast_pseudo_rand_int(0, 6_GB / io_size) * io_size;
                    dsm->prepare_read(rdma_buffer.buffer,
                                      addr,
                                      io_size,
                                      false /* on chip */,
                                      &ctx);
                }
                dsm->commit(&ctx);
                token->complete_task(batch_size);
                tls.op += batch_size;
            }
            dsm->put_rdma_buffer(std::move(rdma_buffer));
        }
    }

private:
    DSM::pointer dsm_;
    std::vector<void *> huge_pages_;
    std::vector<ibv_mr *> irrelevant_mr_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    // f.configure_thread_nr({1, 4, 8, 16});
    f.configure_thread_nr({kMaxAppThread});
    f.configure_coro_nr({8});
    f.add_option<size_t>("io_size", {8});
    f.add_option<size_t>("batch_size", {4});
    f.add_option<bool>("attacker", {true, false});
    f.add_option<size_t>("irrelevant_mr", {0, 1024, 4096});
    auto configs = f.generate_configs();

    exp.configure_monitor(1s, 8);
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}