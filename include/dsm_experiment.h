#pragma once
#include <chrono>

#include "DSM.h"
#include "bench/experiment.h"
#include "rdmacpp/WRCtx.h"

using namespace std::chrono_literals;

namespace bench
{
template <typename Spec, typename ConfigT = QuickBenchConfig>
class DSMExperiment : public ::bench::IExperiment<Spec, ConfigT>
{
public:
    using Config = ConfigT;
    using Base = ::bench::IExperiment<Spec, Config>;
    using StorageT = typename Base::StorageT;
    /**
     * NOTE: After the construction of DSMExperiment,
     * the user should make get_dsm works
     * I.e., construct DSM in the ctor.
     */
    virtual DSM::pointer get_dsm() const
    {
        LOG(FATAL) << "** no get_dsm overloaded.";
    }

    void cluster_barrier(const std::string &key) override
    {
        __dsm().keeper_barrier("__expr:" + key, 100ms);
    }
    int node_id() override
    {
        return __dsm().get_node_id();
    }
    DSM &__dsm()
    {
        auto dsm = get_dsm();
        if (unlikely(dsm == nullptr))
        {
            LOG(FATAL)
                << "get_dsm() got nullptr. Did you construct DSM at the ctor?";
        }
        return *dsm;
    }
    std::optional<uint64_t> cluster_sum(
        uint64_t local, std::chrono::nanoseconds timeout) override
    {
        return __dsm().sum(local, timeout);
    }
    void exit() override
    {
        __dsm().keeper_barrier("__expr_ctl:exit", 100ms);

        Base::exit();
    }
    void thread_init(StorageT &store, size_t thread_nr) override
    {
        if (!__dsm().hasRegistered())
        {
            __dsm().registerThread();
        }
        Base::thread_init(store, thread_nr);
    }
    void on_start_bench(StorageT &store, const Config &config) override
    {
        Base::on_start_bench(store, config);
    }
    void on_end_bench(const ::bench::ResultRecord &results,
                      StorageT &store,
                      const Config &config) override
    {
        LOG(INFO) << PRE(results);
        Base::on_end_bench(results, store, config);
    }

private:
};

template <typename Spec, typename Config = QuickBenchConfig>
class DSMCoroExperiment : public DSMExperiment<Spec, Config>
{
public:
    using Base = ::bench::DSMExperiment<Spec, Config>;
    using StorageT = typename Base::StorageT;

    virtual void benchmark_coroutine(StorageT &,
                                     const Config &,
                                     CoroContext &,
                                     ::bench::StopToken::pointer,
                                     bool)
    {
        LOG(FATAL) << "** no benchmark_coroutine provided.";
    }

    void benchmark_worker(StorageT &store,
                          const Config &conf,
                          CoroContext &ctx,
                          ::bench::StopToken::pointer token,
                          bool is_master) override final
    {
        auto coro_id = ctx.coro_id();
        benchmark_coroutine(store, conf, ctx, token, is_master);
        finished_coro_.current()[coro_id] = true;
        finished_nr_.current()++;
        [[maybe_unused]] auto old = total_finished_nr_.fetch_add(1);
    }
    void on_start_bench(StorageT &sto, const Config &conf) override
    {
        total_finished_nr_ = 0;
        Base::on_start_bench(sto, conf);
    }
    void benchmark_master(StorageT &sto,
                          const Config &conf,
                          CoroContext &mctx,
                          ::bench::StopToken::pointer token,
                          bool b) override
    {
        return default_benchmark_master(sto, conf, mctx, token, b);
    }

protected:
    Perthread<std::vector<bool>> finished_coro_;
    Perthread<size_t> finished_nr_;
    std::atomic<size_t> total_finished_nr_;

private:
    void default_benchmark_master(
        StorageT &,
        const Config &conf,
        CoroContext &mctx,
        [[maybe_unused]] ::bench::StopToken::pointer token,
        bool)
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

        while (total_finished_nr_.load(std::memory_order_relaxed) <
               total_coro_nr)
        {
            _dsm.try_master_coro_poll(&mctx);

            // make these coros prioritized
            auto hot_waiting_coro = mctx.next_hot_waiting_coro();
            if (hot_waiting_coro)
            {
                mctx.yield_to_worker(*hot_waiting_coro);
            }
        }

        // CHECK(token->stop_requested());
    }
};

}  // namespace bench