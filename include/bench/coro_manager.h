#pragma once
#include "./base_config.h"
#include "./manager.h"
#include "./token.h"
#include "CoroContext.h"

/**
 * CoroManager extends the basic Manager by adding/modifying several phases.
 *
 * The comprehensive workloads are: (modified/added are marked with *)
 *
 * init_hook()
 * thread_init_hook()
 * * coro_init_hook()
 *
 * for each configs:
 *     start_bench_hook()
 *     thread_start_bench_hook()
 *     * coro_start_bench_hook()
 *
 *     * benchmark_worker() / benchmark_master()
 *
 *     * coro_end_bench_hook()
 *     thread_end_bench_hook()
 *     end_bench_hook()
 * end for
 *
 * exit_hook()
 *
 * @type Spec from Spec we further retrieve the following types
 * - CLS: per-coroutine object that live each unique bench, ready from
 * coro_start_bench to coro_end_bench
 * - PCLS: per-coroutine object that live across whole experiment, ready from
 * coro
 * @type Config must inherit from BaseConfig.
 */

namespace bench::details
{
template <typename Spec, typename Config, typename Experiment>
requires std::is_base_of_v<IBenchConfig, Config> class CoroManager
    : public Manager<Spec, Config, Experiment>
{
public:
    using ManagerT = Manager<Spec, Config, Experiment>;
    using StorageT = Storage<Spec>;
    using GLS = spec_gls_t<Spec>;
    using TLS = spec_tls_t<Spec>;
    using CLS = spec_cls_t<Spec>;
    using PCLS = spec_pcls_t<Spec>;

    constexpr static size_t V = ManagerT::V;

    using pointer = std::unique_ptr<CoroManager<Spec, Config, Experiment>>;
    static auto new_instance(size_t thread_nr, size_t coro_nr, Experiment &ex)
    {
        return std::make_unique<CoroManager<Spec, Config, Experiment>>(
            thread_nr, coro_nr, ex);
    }
    CoroManager(size_t thread_nr, size_t coro_nr, Experiment &ex)
        : ManagerT(thread_nr, ex), coro_nr_(coro_nr)
    {
        CHECK_LE(thread_nr, kMaxAppThread);
    }
    void thread_init_hook(StorageT &store, size_t thread_nr) override
    {
        ManagerT::thread_init_hook(store, thread_nr);
        // N.B. coro init after thread init

        auto &pclss = persistent_coro_stores_.current();
        for (size_t i = 0; i < coro_nr(); ++i)
        {
            pclss.emplace_back(std::make_unique<PCLS>());
        }
        auto &mpcls = master_persistent_coro_store_.current();
        mpcls = std::make_unique<PCLS>();

        for (size_t i = 0; i < coro_nr(); ++i)
        {
            CoroStorage<Spec> cls(nullptr /* gls */,
                                  nullptr /* tls */,
                                  &store.persistent_thread() /* ptls */,
                                  nullptr /* cls */,
                                  pclss[i].get() /* pcls */);
            cls.modified_id() = store.id();
            cls.modified_id().coro_id = i;
            coro_init_hook(cls, coro_nr());
        }

        CoroStorage<Spec> mcls(nullptr /* gls */,
                               nullptr /* tls */,
                               &store.persistent_thread() /* ptls */,
                               nullptr /* cls */,
                               mpcls.get() /* pcls */);
        mcls.modified_id() = store.id();
        mcls.modified_id().coro_id = kMasterCoro;  // master
        coro_init_hook(mcls, coro_nr());
    }
    void thread_start_bench_hook(StorageT &store, const Config &config) override
    {
        ManagerT::thread_start_bench_hook(store, config);

        // N.B. coro hook starts after thread
        auto &pclss = persistent_coro_stores_.current();
        size_t coro_nr = config.coro_nr();
        auto &clss = coro_stores_.current();
        clss.clear();
        for (size_t i = 0; i < config.coro_nr(); ++i)
        {
            clss.emplace_back(std::make_unique<CLS>());
        }
        master_coro_store_.current() = std::make_unique<CLS>();

        for (size_t i = 0; i < coro_nr; ++i)
        {
            CoroStorage<Spec> cls(&store.global(),
                                  &store.thread(),
                                  &store.persistent_thread(),
                                  clss[i].get(),
                                  pclss[i].get());
            cls.modified_id() = store.id();
            cls.modified_id().coro_id = i;
            coro_start_bench_hook(cls, config);
        }
        // for master coro
        CoroStorage<Spec> mcls(&store.global(),
                               &store.thread(),
                               &store.persistent_thread(),
                               master_coro_store_.current().get(),
                               master_persistent_coro_store_.current().get());
        mcls.modified_id() = store.id();
        mcls.modified_id().coro_id = kMasterCoro;  // master
        coro_start_bench_hook(mcls, config);
    }
    void thread_end_bench_hook(StorageT &store, const Config &config) override
    {
        size_t coro_nr = config.coro_nr();
        auto &clss = coro_stores_.current();
        auto &pclss = persistent_coro_stores_.current();
        for (size_t i = 0; i < coro_nr; ++i)
        {
            CoroStorage<Spec> cls(&store.global(),
                                  &store.thread(),
                                  &store.persistent_thread(),
                                  clss[i].get(),
                                  pclss[i].get());
            cls.modified_id() = store.id();
            cls.modified_id().coro_id = i;
            coro_end_bench_hook(cls, config);
        }
        // for master coro
        CoroStorage<Spec> mcls(&store.global(),
                               &store.thread(),
                               &store.persistent_thread(),
                               master_coro_store_.current().get(),
                               master_persistent_coro_store_.current().get());
        mcls.modified_id() = store.id();
        mcls.modified_id().coro_id = kMasterCoro;  // master
        coro_end_bench_hook(mcls, config);

        ManagerT::thread_end_bench_hook(store, config);
    }

    void benchmark_hook(StorageT &store,
                        const Config &config,
                        StopToken::pointer token,
                        bool is_master) override
    {
        do_coro_benchmark(store, config, token, is_master);
    }
    virtual ~CoroManager() = default;

protected:
    virtual void coro_init_hook(StorageT &store, size_t coro_nr)
    {
        this->get_ex().coro_init(store, coro_nr);
    }
    virtual void coro_start_bench_hook(StorageT &store, const Config &conf)
    {
        this->get_ex().on_coro_start_bench(store, conf);
    }
    virtual void coro_end_bench_hook(StorageT &store, const Config &config)
    {
        this->get_ex().on_coro_end_bench(store, config);
    }

    constexpr size_t coro_nr() const
    {
        return coro_nr_;
    }

private:
    size_t coro_nr_{0};
    Perthread<std::vector<std::unique_ptr<CLS>>> coro_stores_;
    Perthread<std::unique_ptr<CLS>> master_coro_store_;

    Perthread<std::vector<std::unique_ptr<PCLS>>> persistent_coro_stores_;
    Perthread<std::unique_ptr<PCLS>> master_persistent_coro_store_;

    void do_coro_benchmark(StorageT &store,
                           const IBenchConfig &config,
                           StopToken::pointer token,
                           bool is_master)
    {
        VLOG(V)
            << "[coro_manager] entering coro_do_bench_thread: start of coro "
               "lifecycle";
        CoroCall workers[define::kMaxCoroNr];
        CoroCall master;

        CHECK_LE(config.coro_nr(), define::kMaxCoroNr);
        CHECK_GT(config.coro_nr(), 0);

        // The reousrce is guarded by unique_ptr, but give
        // raw pointers to CoroContext.
        // In order to avoid catching smart pointer in lambda.
        auto coro_cb = CoroControlBlock::make_ptr();

        for (size_t i = 0; i < config.coro_nr(); ++i)
        {
            workers[i] = CoroCall([i,
                                   &master,
                                   &store,
                                   &config,
                                   is_master,
                                   token,
                                   cb = coro_cb.get(),
                                   this](CoroYield &yield) {
                do_worker_coro_benchmark_impl(
                    i, yield, cb, &master, store, config, token, is_master);
            });
        }
        master = CoroCall([&workers,
                           &store,
                           &config,
                           token,
                           is_master,
                           cb = coro_cb.get(),
                           this](CoroYield &yield) {
            do_master_coro_benchmark_impl(
                yield, workers, cb, store, token, config, is_master);
        });

        master();
        VLOG(V) << "[coro_manager] leaving coro_do_bench_thread: end of coro "
                   "lifecycle";
    }

    void do_worker_coro_benchmark_impl(size_t coro_id,
                                       CoroYield &yield,
                                       CoroControlBlock *cb,
                                       CoroCall *master,
                                       StorageT &store,
                                       const IBenchConfig &config,
                                       ::bench::StopToken::pointer finished,
                                       bool is_master)
    {
        VLOG(V) << "[coro_manager] entering worker coro(" << coro_id << ")";
        const Config &actual_config = dynamic_cast<const Config &>(config);

        auto tid = util::get_thread_id();

        CoroContext ctx(tid, &yield, master, coro_id, cb);
        auto &clss = coro_stores_.current();
        auto &pclss = persistent_coro_stores_.current();

        CoroStorage<Spec> cls(&store.global(),
                              &store.thread(),
                              &store.persistent_thread(),
                              clss[coro_id].get(),
                              pclss[coro_id].get());
        cls.modified_id() = store.id();
        cls.modified_id().coro_id = coro_id;

        VLOG(V) << "[coro_manager] entering worker coro(" << coro_id << ")";
        do_worker_benchmark(cls, actual_config, ctx, finished, is_master);
        VLOG(V) << "[coro_manager] leaving worker coro(" << coro_id << ")";

        ctx.record_yield_reason(true /* exit */);
        ctx.yield_to_master();

        LOG(FATAL) << "** unreachable.";
    }

    virtual void do_worker_benchmark(StorageT &store,
                                     const Config &config,
                                     CoroContext &ctx,
                                     ::bench::StopToken::pointer stop_token,
                                     bool is_master)
    {
        return ManagerT::get_ex().benchmark_worker(
            store, config, ctx, stop_token, is_master);
    }

    void do_master_coro_benchmark_impl(CoroYield &yield,
                                       CoroCall *workers,
                                       CoroControlBlock *cb,
                                       StorageT &store,
                                       StopToken::pointer finished,
                                       const IBenchConfig &config,
                                       bool is_master)
    {
        VLOG(V) << "[coro_manager] entering master coro";
        const Config &actual_config = dynamic_cast<const Config &>(config);

        auto tid = util::get_thread_id();
        CoroContext mctx(tid, &yield, workers, cb);
        CHECK(mctx.is_master());

        CoroStorage<Spec> mcls(&store.global(),
                               &store.thread(),
                               &store.persistent_thread(),
                               master_coro_store_.current().get(),
                               master_persistent_coro_store_.current().get());

        mcls.modified_id() = store.id();
        mcls.modified_id().coro_id = kMasterCoro;  // master
        do_master_benchmark(mcls, actual_config, mctx, finished, is_master);

        // CHECK(finished->stop_requested());

        VLOG(V) << "[coro_manager] leaving master coro";
    }
    virtual void do_master_benchmark(StorageT &store,
                                     const Config &config,
                                     CoroContext &ctx,
                                     ::bench::StopToken::pointer stop_token,
                                     bool is_master)
    {
        ManagerT::get_ex().benchmark_master(
            store, config, ctx, stop_token, is_master);
    }
};
}  // namespace bench::details