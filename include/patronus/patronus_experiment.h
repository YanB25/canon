#pragma once
#include <chrono>

#include "DSM.h"
#include "IOVerbose.h"
#include "PerThread.h"
#include "bench/experiment.h"
#include "patronus/Patronus.h"

namespace bench
{
template <typename Spec, typename Config>
class IPatronusExperiment : public IExperiment<Spec, Config>
{
public:
    static constexpr size_t V = ::config::verbose::kCoroLauncher;

    using Base = IExperiment<Spec, Config>;
    using BLS = typename Base::BLS;
    IPatronusExperiment() = default;

    virtual ::patronus::Patronus::pointer get_patronus() const
    {
        LOG(FATAL) << "** no get_patronus overloaded";
    }
    virtual bool is_client() const
    {
        auto p = get_patronus();
        auto nid = p->get_node_id();
        return ::config::is_client(nid);
    }

    void thread_init(BLS &bls, size_t thread_nr) override
    {
        auto p = get_patronus();
        if (is_client())
        {
            p->registerClientThread();
            CHECK_LT(p->get_thread_id(), kMaxAppThread);
        }
        else
        {
            p->registerServerThread();
            CHECK_LT(p->get_thread_id(), NR_DIRECTORY);
        }
        Base::thread_init(bls, thread_nr);
    }

    std::optional<uint64_t> cluster_sum(
        uint64_t val, std::chrono::nanoseconds timeout) override
    {
        auto p = get_patronus();
        auto dsm = p->get_dsm();
        return dsm->sum(val, timeout);
    }

    void cluster_barrier(const std::string &key) override
    {
        auto p = get_patronus();
        auto dsm = p->get_dsm();
        return dsm->keeper_barrier(key, 100ms);
    }

    void on_end_bench(const ResultRecord &results,
                      BLS &bls,
                      const Config &config) override
    {
        auto p = get_patronus();
        if (is_client())
        {
            p->finished(wait_key_);
        }
        else
        {
            // do nothing
        }
        wait_key_++;
        Base::on_end_bench(results, bls, config);
    }

    void benchmark(BLS &bls,
                   const Config &config,
                   StopToken::pointer stop_token,
                   bool is_master) override
    {
        std::ignore = bls;
        std::ignore = config;
        std::ignore = stop_token;
        std::ignore = is_master;
        CHECK(!is_client());
        auto p = get_patronus();
        p->server_serve(wait_key_);
        CHECK(stop_token->stop_requested());
    }
    void on_start_bench(BLS &bls, const Config &config) override
    {
        if (is_client())
        {
            // do nothing
        }
        else
        {
            auto p = get_patronus();
            p->finished(wait_key_);
        }
        Base::on_start_bench(bls, config);
    }

    void benchmark_worker(BLS &bls,
                          const Config &config,
                          CoroContext &ctx,
                          ::bench::StopToken::pointer stop_token,
                          bool is_master) override
    {
        CHECK(is_client());
        VLOG(V) << "[patronus_manager] entering worker coro.";
        auto tid = util::get_thread_id();
        auto coro_id = ctx.coro_id();

        auto p = get_patronus();
        DCHECK_EQ(tid, p->get_thread_id());

        auto &finish_all_task = finish_all_tasks_[tid];
        CHECK(!finish_all_task[coro_id]);
        auto &finish_nr = coro_finished_nr_[tid];

        VLOG(V) << "[patronus_manager] entering task_f " << ctx;

        patronus_worker(p, bls, config, ctx, stop_token, is_master);

        finish_all_task[coro_id] = true;
        finish_nr++;
        VLOG(V) << "[patronus_manager] coro finishing all the tasks. "
                   "leaving... "
                << ctx << ", " << util::pre(finish_all_task);
        ctx.record_yield_reason(true /* exit */);
        ctx.yield_to_master();
        LOG(FATAL) << "** not reachable.";
    }

    virtual void patronus_worker(patronus::Patronus::pointer,
                                 BLS &,
                                 const Config &,
                                 CoroContext &,
                                 ::bench::StopToken::pointer,
                                 bool)
    {
        LOG(FATAL) << "** no patronus_worker overloaded.";
    }
    void exit() override
    {
        auto p = get_patronus();
        auto dsm = p->get_dsm();
        dsm->keeper_barrier("__exit", 100ms);
        LOG(INFO) << "Exited.";
        Base::exit();
    }

    void benchmark_master(BLS &bls,
                          const Config &config,
                          CoroContext &mctx,
                          ::bench::StopToken::pointer token,
                          bool is_master) override
    {
        CHECK(is_client());
        std::ignore = bls;
        std::ignore = config;
        std::ignore = is_master;

        auto p = get_patronus();

        auto tid = util::get_thread_id();
        CHECK(mctx.is_master());

        auto &finish_all_task = finish_all_tasks_[tid];
        finish_all_task.clear();
        finish_all_task.resize(config.coro_nr(), false);

        auto &finish_nr = coro_finished_nr_[tid];
        finish_nr = 0;

        for (size_t i = 0; i < config.coro_nr(); ++i)
        {
            mctx.yield_to_worker(i);
        }
        coro_t coro_buf[2 * define::kMaxCoroNr];

        while (finish_nr != config.coro_nr())
        {
            DCHECK_LE(finish_nr, config.coro_nr());

            auto nr = p->try_get_client_continue_coros(coro_buf,
                                                       2 * define::kMaxCoroNr);
            for (size_t i = 0; i < nr; ++i)
            {
                auto coro_id = coro_buf[i];
                mctx.yield_to_worker(coro_id);
            }
        }
        DCHECK_LE(finish_nr, config.coro_nr());
        CHECK(std::all_of(std::begin(finish_all_task),
                          std::end(finish_all_task),
                          [](bool i) { return i; }));
        CHECK(token->stop_requested());
    }
    virtual ~IPatronusExperiment() = default;

private:
    uint64_t wait_key_{0};

    Perthread<std::vector<bool>> finish_all_tasks_;
    Perthread<uint64_t> coro_finished_nr_{};
};

// template <typename ThreadContext, typename Config>
// class IPatronusServerExperiment : public IExperiment<ThreadContext, Config>
// {
// public:
//     using Base = IExperiment<ThreadContext, Config>;
//     IPatronusServerExperiment(const patronus::PatronusConfig &config)
//         : config_(config)
//     {
//     }
//     virtual ::patronus::Patronus::pointer get_patronus()
//     {
//         LOG(FATAL) << "** no get_patronus overloaded";
//     }
//     // void thread_init(ThreadContext &tlc) override
//     // {
//     //     auto p = get_patronus();
//     //     p->registerServerThread();
//     //     CHECK_LT(p->get_thread_id(), NR_DIRECTORY);
//     //     Base::thread_init(tlc);
//     // }
//     // void on_start_bench(const Config &config) override
//     // {
//     //     auto p = get_patronus();
//     //     p->finished(wait_key_);
//     //     Base::on_start_bench(config);
//     // }

//     // uint64_t cluster_sum(uint64_t val) override
//     // {
//     //     auto p = get_patronus();
//     //     auto dsm = p->get_dsm();
//     //     return dsm->sum(val);
//     // }
//     // void cluster_barrier(const std::string &key) override
//     // {
//     //     auto p = get_patronus();
//     //     auto dsm = p->get_dsm();
//     //     return dsm->keeper_barrier(key, 100ms);
//     // }

//     void on_end_bench(const ResultRecord &results,
//                       const Config &config) override
//     {
//         wait_key_++;
//         Base::on_end_bench(results, config);
//     }

//     void benchmark(ThreadContext &tlc,
//                    const Config &config,
//                    StopToken::pointer stop_token,
//                    bool is_master) override
//     {
//         std::ignore = tlc;
//         std::ignore = config;
//         std::ignore = stop_token;
//         std::ignore = is_master;
//         auto p = get_patronus();
//         p->server_serve(wait_key_);
//         CHECK(stop_token->stop_requested());
//     }
//     virtual ~IPatronusServerExperiment() = default;

// private:
//     patronus::PatronusConfig config_;
//     patronus::Patronus::pointer p_;
//     DSM::pointer dsm_;

//     uint64_t wait_key_{0};
// };

}  // namespace bench