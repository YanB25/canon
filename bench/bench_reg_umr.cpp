#include <numa.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <thread>

#include "Common.h"
#include "DSMCache.h"
#include "WRLock.h"
#include "bench/DataFrame.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
#include "util/gflags_def.h"

struct Spec
{
    struct CLS
    {
        std::vector<ibv_mr *> allocated_mr;
        std::vector<ibv_mr *> allocated_umr;
        std::vector<void *> allocated_buffer;
        std::vector<ibv_mr *> mr_pool;
        std::vector<ibv_mr *> umr_pool;

        // OnePassBucketMonitor build_m(util::time::to_ns(0ns),
        //                              util::time::to_ns(100ms),
        //                              util::time::to_ns(10us));
        // OnePassBucketMonitor gc_m(util::time::to_ns(0ns),
        //                           util::time::to_ns(100ms),
        //                           util::time::to_ns(10us));
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
        config.worker_nr = 0;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();
    }
    void thread_init(BLS &bls, size_t thread_nr) override
    {
        dsm_->registerThread();
        Base::thread_init(bls, thread_nr);
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        Base::on_start_bench(bls, conf);
    }
    void on_end_bench(const ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        df_.reg_result(res, conf);

        Base::on_end_bench(res, bls, conf);
    }
    void on_coro_end_bench(BLS &bls, const Config &conf) override
    {
        auto &cls = bls.coroutine();

        bool mr_reuse = conf.get<bool>("mr_reuse");
        gc_api(bls, mr_reuse);

        for (auto *mr : cls.mr_pool)
        {
            do_deallocate_mr(mr);
        }
        cls.mr_pool.clear();
        for (auto *umr : cls.umr_pool)
        {
            do_deallocate_umr(umr);
        }
        cls.umr_pool.clear();
    }

    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        Base::on_thread_end_bench(bls, conf);
    }

    void exit() override
    {
        df_.dump(FLAGS_binary, FLAGS_exec_meta);
    }

    void *allocate(size_t size)
    {
        return malloc(size);
    }
    void deallocate(void *addr)
    {
        return free(addr);
    }

    void benchmark_master(StorageT &,
                          const Config &conf,
                          CoroContext &mctx,
                          ::bench::StopToken::pointer token,
                          bool) override
    {
        size_t coro_nr = conf.coro_nr();
        size_t total_coro_nr = conf.effective_client_nr();

        finished_coro_.current().clear();
        finished_coro_.current().resize(coro_nr, false);
        finished_nr_.current() = 0;

        auto &_dsm = this->__dsm();

        for (size_t i = 0; i < coro_nr; ++i)
        {
            mctx.yield_to_worker(i);
        }

        auto dir_id = 0;
        auto *cq = _dsm.get_dir_cq(dir_id);
        ibv_exp_wc wcs[32];
        while (total_finished_nr_.load(std::memory_order_relaxed) <
               total_coro_nr)
        {
            int ret = ibv_exp_poll_cq(cq, 32, wcs, sizeof(wcs[0]));
            for (int i = 0; i < ret; ++i)
            {
                auto &wc = wcs[i];
                auto *wr_ctx = (rdma::WRCtx *) wc.wr_id;
                auto &wr = wr_ctx->wr();
                if (likely(wc.status == IBV_WC_SUCCESS))
                {
                    wr_ctx->pp_((void *) wr_ctx);
                    if (wr_ctx->ctx_)
                    {
                        mctx.yield_to_worker(wr_ctx->ctx_->coro_id());
                    }
                }
                else
                {
                    LOG(FATAL) << std::endl
                               << PRE(wc) << std::endl
                               << PRE(wr) << ". Die.";
                }
            }

            // make these coros prioritized
            auto hot_waiting_coro = mctx.next_hot_waiting_coro();
            if (hot_waiting_coro)
            {
                mctx.yield_to_worker(*hot_waiting_coro);
            }
        }

        CHECK(token->stop_requested());
    }

    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        auto mr_size = conf.get<size_t>("mr_size");
        auto mr_depth = conf.get<size_t>("mr_depth");
        auto mr_width = conf.get<size_t>("mr_width");
        bool mr_reuse = conf.get<bool>("mr_reuse");
        auto cls = bls.coroutine();
        std::ignore = ctx;
        auto dsm = get_dsm();
        ChronoTimer timer;
        while (!token->stop_requested())
        {
            timer.pin();
            [[maybe_unused]] auto *umr =
                build_umr(mr_size, mr_depth, mr_width, bls, mr_reuse, ctx);
            auto build_ns = timer.pin();

            gc_api(bls, mr_reuse);
            [[maybe_unused]] auto destroy_ns = timer.pin();
            token->complete_task(1);

            token->collect_ns(build_ns);
        }
    }

    void gc_api(BLS &bls, bool reuse_mr)
    {
        auto &cls = bls.coroutine();

        for (auto *mr : cls.allocated_mr)
        {
            deallocate_mr_api(bls, mr, reuse_mr);
        }
        cls.allocated_mr.clear();
        for (auto *umr : cls.allocated_umr)
        {
            deallocate_umr_api(bls, umr, reuse_mr);
        }
        cls.allocated_umr.clear();
        for (auto *buff : cls.allocated_buffer)
        {
            deallocate(buff);
        }
        cls.allocated_buffer.clear();
    }

    ibv_mr *do_allocate_mr(size_t size, BLS &bls)
    {
        // LOG(INFO) << "do allocate mr";
        auto &cls = bls.coroutine();
        auto dir_id = 0;
        uint32_t access_flags = access_flags = IBV_ACCESS_LOCAL_WRITE |
                                               IBV_ACCESS_REMOTE_WRITE |
                                               IBV_ACCESS_REMOTE_READ;

        auto *rdma_ctx = dsm_->get_dir_rdma_context(dir_id);
        auto *addr = allocate(size);
        cls.allocated_buffer.push_back(addr);
        auto *mr = ibv_reg_mr(rdma_ctx->pd, addr, size, access_flags);
        CHECK_EQ(mr->addr, addr);
        CHECK_EQ(mr->length, size);
        bls.coroutine().allocated_mr.push_back(mr);
        return CHECK_NOTNULL(mr);
    }

    void do_deallocate_mr(ibv_mr *mr)
    {
        dsm_->destroy_mr(mr);
    }
    ibv_mr *do_allocate_umr(size_t dir_id, size_t wide, BLS &bls)
    {
        // LOG(INFO) << "!! allocating umr";
        auto &cls = bls.coroutine();
        auto *umr = dsm_->create_umr(dir_id, wide);
        cls.allocated_umr.push_back(umr);
        return umr;
    }
    void do_deallocate_umr(ibv_mr *umr)
    {
        dsm_->destroy_mr(umr);
    }

    ibv_mr *allocate_mr_api(BLS &bls, size_t size, bool reuse)
    {
        auto &cls = bls.coroutine();
        if (reuse && !cls.mr_pool.empty())
        {
            auto &cls = bls.coroutine();
            auto *ret = cls.mr_pool.back();
            cls.mr_pool.pop_back();
            cls.allocated_mr.push_back(ret);
            return ret;
        }
        else
        {
            return do_allocate_mr(size, bls);
        }
    }
    void deallocate_mr_api(BLS &bls, ibv_mr *mr, bool reuse)
    {
        if (reuse)
        {
            // LOG(INFO) << "!! deallocate";
            auto &cls = bls.coroutine();
            cls.mr_pool.push_back(mr);
        }
        else
        {
            do_deallocate_mr(mr);
        }
    }

    ibv_mr *allocate_umr_api(BLS &bls, size_t dir_id, size_t wide, bool reuse)
    {
        auto &cls = bls.coroutine();
        if (reuse && !cls.umr_pool.empty())
        {
            auto &cls = bls.coroutine();
            auto *ret = cls.umr_pool.back();
            cls.umr_pool.pop_back();
            cls.allocated_umr.push_back(ret);
            return ret;
        }
        return do_allocate_umr(dir_id, wide, bls);
    }
    void deallocate_umr_api(BLS &bls, ibv_mr *umr, bool reuse)
    {
        if (reuse)
        {
            auto &cls = bls.coroutine();
            cls.umr_pool.push_back(umr);
        }
        else
        {
            do_deallocate_umr(umr);
        }
    }

    ibv_mr *build_umr(size_t mr_size,
                      size_t level,
                      size_t wide,
                      BLS &bls,
                      bool reuse,
                      CoroContext &ctx)
    {
        auto to_nid = dsm_->get_node_id();
        auto to_tid = dsm_->get_thread_id();
        auto dir_id = 0;

        if (level == 0)
        {
            // base level
            auto *mr = allocate_mr_api(bls, mr_size, reuse);
            return mr;
        }
        else
        {
            auto *umr = allocate_umr_api(bls, dir_id, wide, reuse);

            std::vector<ibv_exp_mem_region> mem_region;
            for (size_t i = 0; i < wide; ++i)
            {
                auto *base_mr =
                    build_umr(mr_size, level - 1, wide, bls, reuse, ctx);
                mem_region.emplace_back(ibv_exp_mem_region{
                    .base_addr = (uint64_t) base_mr->addr,
                    .mr = base_mr,
                    .length = base_mr->length,
                });
            }

            dsm_->prepare_reg_list_umr(umr,
                                       to_nid,
                                       to_tid,
                                       dir_id,
                                       mem_region.data(),
                                       wide,
                                       std::nullopt /*base addr*/,
                                       &ctx);
            dsm_->commit(&ctx);

            return umr;
        }
    }

private:
    DSM::pointer dsm_;

    bench::ResultDataFrame df_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    // Only one thread is allowed,
    // because thread_nr == NR_DIRECTORY
    // and we currently just test one thread.
    f.configure_thread_nr({1});
    f.configure_coro_nr({1, 2, 4});

    f.add_option<size_t>("mr_size", {4_MB});
    // f.add_option<size_t>("mr_size", {64});
    f.add_option<size_t>("mr_depth", {1});
    f.add_option<size_t>("mr_width", {20});
    // f.add_option<bool>("mr_reuse", {true, false});
    f.add_option<bool>("mr_reuse", {true});

    auto configs = f.generate_configs();

    exp.configure_monitor(500ms, 6);
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}