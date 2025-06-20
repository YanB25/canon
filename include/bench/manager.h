#pragma once
#include <functional>
#include <ratio>
#include <thread>

#include "./base_config.h"
#include "Common.h"
#include "base_experiment.h"
#include "bench/result.h"
#include "bench/token.h"
#include "boost/thread/barrier.hpp"
#include "util/Numa.h"
#include "util/Pre.h"

namespace bench::details
{
/**
 * workflow
 *
 * init_hook()
 * thread_init_hook()
 *
 * for each configs:
 *     start_bench_hook()
 *     thread_start_bench_hook()
 *
 *     benchmark()
 *
 *     thread_end_bench_hook()
 *     end_bench_hook()
 * end for
 *
 * exit_hook()
 *
 * @type Spec from Spec we retrieve the following types
 * - GLS: globally shared object that live each unique bench, ready from
 * start_bench to end_bench
 * - TLS: per-thread object that live each unique bench, ready from
 * thread_start_bench to thread_end_bench
 * - PTLS: per-thread object that live across whole experiment, ready from
 * thread_init.
 * @type Config is any type inheriting BaseConfig.
 */

template <typename Spec, typename Config, typename Experiment>
requires std::is_base_of_v<IBenchConfig, Config>
    &&std::is_base_of_v<IExperiment<Spec, Config>, Experiment> class Manager
{
public:
    using StorageT = Storage<Spec>;
    using GLS = typename StorageT::GLS;
    using TLS = typename StorageT::TLS;
    using PTLS = typename StorageT::PTLS;

    constexpr static size_t V = config::verbose::kCoroLauncher;
    using pointer = std::unique_ptr<Manager<Spec, Config, Experiment>>;
    static auto new_instance(size_t thread_nr, Experiment &ex)
    {
        return std::make_unique<Manager<Spec, Config, Experiment>>(thread_nr,
                                                                   ex);
    }
    Manager(size_t thread_nr, Experiment &ex)
        : ex_(ex), thread_nr_(thread_nr), bar_(thread_nr_)
    {
        CHECK_LE(thread_nr, kMaxAppThread);
    }

    using MonitorF = std::function<ResultRecord(const Config &,
                                                ::bench::StopToken::pointer)>;
    virtual void register_monitor(const MonitorF &monitor_f)
    {
        CHECK(!monitor_f_.has_value());
        monitor_f_ = monitor_f;
    }

    template <typename R1, typename P1>
    void register_default_monitor(std::chrono::duration<R1, P1> interval,
                                  size_t epoch_nr,
                                  bool report_history)
    {
        register_monitor(
            [this, interval, epoch_nr, report_history](
                const Config &,
                ::bench::StopToken::pointer proxy) -> ::bench::ResultRecord {
                return this->monitor_template(
                    interval, epoch_nr, proxy, report_history);
            });
    }

    std::optional<uint64_t> do_cluster_sum(uint64_t val,
                                           std::chrono::nanoseconds timeout)
    {
        return get_ex().cluster_sum(val, timeout);
    }

    virtual void bench(
        const std::vector<std::shared_ptr<IBenchConfig>> &configs)
    {
        barrier_nodes_hook("__manager:before_init");

        init_hook();

        barrier_nodes_hook("__manager:after_init");

        CHECK_GE(kMaxAppThread, thread_nr_);
        std::vector<std::thread> threads;
        for (size_t i = 1; i < thread_nr_; ++i)
        {
            threads.emplace_back([&configs, this]() { bench_thread(configs); });
        }

        bench_thread(configs);

        for (auto &t : threads)
        {
            t.join();
        }

        barrier_nodes_hook("__manager:before_exit");
        exit_hook();
        barrier_nodes_hook("__manager:after_exit");
    }

    Experiment &get_ex()
    {
        return ex_;
    }
    const Experiment &get_ex() const
    {
        return ex_;
    }

    template <typename A, typename B, typename C>
    friend std::ostream &operator<<(std::ostream &, const Manager<A, B, C> &);

    virtual ~Manager() = default;

    void set_enable_monitor(bool enable_monitor)
    {
        enable_monitor_ = enable_monitor;
    }

protected:
    virtual void init_hook()
    {
        get_ex().init();
    }
    virtual void thread_init_hook(StorageT &store, size_t thread_nr)
    {
        // util::get_thread_id() is unique in the whole process,
        // which is used to bind core
        auto &numa_ctl = util::NUMACtl::tl_ins();
        CHECK(
            numa_ctl.try_set_core_affinity_by_thread_id(util::get_thread_id()));

        get_ex().thread_init(store, thread_nr);
    }
    virtual void start_bench_hook(StorageT &store, const Config &config)
    {
        exiter_core_.reset();
        exiter_core_ = std::make_unique<StopTokenCore>();

        get_ex().on_start_bench(store, config);
    }

    virtual void thread_start_bench_hook(StorageT &store, const Config &config)
    {
        get_ex().on_thread_start_bench(store, config);
    }

    virtual void benchmark_hook(StorageT &store,
                                const Config &config,
                                ::bench::StopToken::pointer token,
                                bool is_master)
    {
        get_ex().benchmark(store, config, token, is_master);
    }

    virtual void thread_end_bench_hook(StorageT &store, const Config &config)
    {
        get_ex().on_thread_end_bench(store, config);
    }

    virtual void end_bench_hook(const ResultRecord &results,
                                StorageT &store,
                                const Config &config)
    {
        get_ex().on_end_bench(results, store, config);

        exiter_core_.reset();
        exiter_core_ = nullptr;
    }

    virtual void exit_hook()
    {
        get_ex().exit();
    }

    virtual void barrier_threads_hook()
    {
        do_barrier_threads();
    }
    virtual void barrier_nodes_hook(const std::string &name)
    {
        do_barrier_nodes(name);
    }

    constexpr size_t thread_nr() const
    {
        return thread_nr_;
    }

private:
    void do_reset_tls(size_t thread_id)
    {
        thread_context_.current() = std::make_unique<Aligned<TLS>>();

        TLS *tls_ptr = &(thread_context_.current().get()->get());
        PTLS *ptls_ptr = ptls_.current().get();
        store_.current() = ThreadStorage<Spec>(gls_.get(), tls_ptr, ptls_ptr);
        auto &id = store_.current().modified_id();
        id.thread_id = thread_id;
        id.node_id = ex_.node_id();
    }
    void do_reset_gls(size_t thread_id)
    {
        gls_ = std::make_unique<GLS>();
        gls_store_ = GlobalStorage<Spec>(gls_.get());
        auto &id = gls_store_.modified_id();
        id.thread_id = thread_id;
        id.node_id = ex_.node_id();
    }

    Experiment &ex_;
    size_t thread_nr_;

    std::unique_ptr<GLS> gls_{};
    GlobalStorage<Spec> gls_store_{};

    Perthread<std::unique_ptr<PTLS>> ptls_{};

    StopTokenCore::pointer exiter_core_;

    std::optional<MonitorF> monitor_f_;

    boost::barrier bar_;
    std::unique_ptr<std::thread> monitor_thread_;

    std::atomic<uint64_t> master_election_{0};

    std::atomic<uint64_t> internal_tid_{0};

    bool enable_monitor_{true};

    // Use Aligned<T> so that is it cache-aglined
    // Use unique_ptr so that T is no need to be copy-able.
    // Use vector so that we adjust @thread_nr of @thread_contexts
    Perthread<std::unique_ptr<Aligned<TLS>>> thread_context_;
    static_assert(sizeof(Aligned<TLS>) % 64 == 0,
                  "Aligned<T> failed to make T cache-aligned");
    Perthread<ThreadStorage<Spec>> store_;

    ResultRecord do_monitor(const Config &config,
                            ::bench::StopToken::pointer proxy)
    {
        if (monitor_f_.has_value())
        {
            return monitor_f_.value()(config, proxy);
        }
        else
        {
            LOG(WARNING) << "[manager] no monitor registered.";
            return {};
        }
    }
    void enter_do_benchmark(StorageT &store,
                            const Config &config,
                            bool is_master)
    {
        VLOG(V) << "[manager] Entering bench_f...";
        auto token = exiter_core_->get_token();
        benchmark_hook(store, config, token, is_master);
        VLOG(V) << "[manager] Leaving bench_f...";
    }

    void do_barrier_threads()
    {
        bar_.wait();
    }

    void do_barrier_nodes(const std::string &name)
    {
        get_ex().cluster_barrier(name);
    }

    void bench_thread(const std::vector<std::shared_ptr<IBenchConfig>> &configs)
    {
        // NOTE: must select thread_is internally
        // instead of calling util::get_thread_id().
        // This is because external threads may make what returns by
        // util::get_thread_id() not contineous.
        auto thread_id = internal_tid_.fetch_add(1);

        // only exactly one of threads is the master
        bool is_master = (master_election_.fetch_add(1) == 0);

        static size_t __times = 0;

        VLOG_IF(V, is_master)
            << "[manager] thread init hook(" << __times << ")";

        barrier_threads_hook();

        // init ptls here, before entering thread_init_hook
        ptls_.current() = std::make_unique<PTLS>();
        ThreadStorage<Spec> tls_store(
            nullptr /* gls */, nullptr /* tls */, ptls_.current().get());

        tls_store.modified_id().thread_id = thread_id;
        tls_store.modified_id().node_id = ex_.node_id();
        // nothing is ready on thread_init
        thread_init_hook(tls_store, thread_nr());

        barrier_threads_hook();

        if (is_master)
        {
            barrier_nodes_hook("manager:enter");
        }
        barrier_threads_hook();

        for (const auto &config : configs)
        {
            const Config &actual_config = dynamic_cast<const Config &>(*config);

            barrier_threads_hook();
            if (is_master)
            {
                auto name =
                    std::string("manager:run-") + std::to_string(__times);
                __times++;
                barrier_nodes_hook(name);
            }
            barrier_threads_hook();

            if (is_master)
            {
                VLOG(V) << "[manager] global start bench hook(" << __times
                        << ")";
                do_reset_gls(thread_id);
                start_bench_hook(gls_store_, actual_config);
            }
            barrier_threads_hook();
            do_reset_tls(thread_id);

            auto &tls_store = store_.current();

            bool thread_should_enter = thread_id < config->thread_nr();
            VLOG_IF(V, is_master)
                << "[manager] thread start bench hook(" << __times << ")";
            if (thread_should_enter)
            {
                thread_start_bench_hook(tls_store, actual_config);
            }
            barrier_threads_hook();

            // Before doing benchmark, do a global sync
            if (is_master)
            {
                auto name = std::string("manager:before-bench-") +
                            std::to_string(__times);
                __times++;
                barrier_nodes_hook(name);
            }
            barrier_threads_hook();

            // result_record is only valid until monitor thread is joined.
            ::bench::ResultRecord result_record;
            // launcher monitor only after start_bench_hook: it init
            // StopTokenCore
            if (is_master && enable_monitor_)
            {
                // NOTE: call get_token() here before entering
                // enter_do_benchmark() because otherwise token's signal of stop
                // will be lost.
                auto *token = exiter_core_->get_token();
                monitor_thread_ = std::make_unique<std::thread>(
                    [this, token, &actual_config, &result_record]() {
                        result_record = do_monitor(actual_config, token);
                    });
            }

            if (thread_should_enter)
            {
                VLOG(V) << "[manager] entering benchmark(" << __times << ")";
                enter_do_benchmark(tls_store, actual_config, is_master);
                VLOG(V) << "[manager] leaving benchmark(" << __times << ")";
            }
            barrier_threads_hook();

            // Right after benchmark: do a sync.
            if (is_master)
            {
                auto name = std::string("manager:after-bench-") +
                            std::to_string(__times);
                __times++;
                barrier_nodes_hook(name);
            }
            barrier_threads_hook();

            VLOG(V) << "[manager] thread end bench hook(" << __times << ")";
            if (thread_should_enter)
            {
                thread_end_bench_hook(tls_store, actual_config);
            }
            barrier_threads_hook();

            // core_ will be freed in @end_bench_hook
            // so, must join the monitor thread before it
            if (is_master)
            {
                monitor_thread_->join();
                monitor_thread_.reset();
                VLOG(V) << "[manager] global end bench hook(" << __times << ")";
                end_bench_hook(result_record, gls_store_, actual_config);
            }
            barrier_threads_hook();

            if (is_master)
            {
                auto name =
                    std::string("manager:finish-") + std::to_string(__times);
                barrier_nodes_hook(name);
            }
            barrier_threads_hook();
        }

        barrier_threads_hook();
        if (is_master)
        {
            master_election_ = 0;
            barrier_nodes_hook("manager:leave");
        }
        barrier_threads_hook();
        VLOG(V) << "[manager] LEAVED.";
    }
    template <typename R1, typename P1>
    ResultRecord monitor_template(std::chrono::duration<R1, P1> interval,
                                  size_t epoch_nr,
                                  ::bench::StopToken::pointer proxy,
                                  bool report_history)
    {
        ChronoTimer timer;
        uint64_t last_finished_nr = 0;
        SequenceLimit<double> perf_full_history(epoch_nr);
        SequenceLimit<double> perf_history(epoch_nr / 2);
        size_t monitor_nr = 0;
        CHECK_GE(epoch_nr, 4)
            << "Too little epoch to get a result that makes sense";

        for (size_t epoch_id = 0; epoch_id < epoch_nr; ++epoch_id)
        {
            std::this_thread::sleep_for(interval);
            // very unlikely: client thread request to stop
            bool force_exit = proxy->stop_requested();

            auto ns = timer.pin();
            auto finished_nr = proxy->global_nr();

            auto cur_finished_nr = finished_nr - last_finished_nr;

            auto opt_cluster_cur_finished_nr =
                do_cluster_sum(cur_finished_nr, interval);
            uint64_t cluster_cur_finished_nr = 0;
            if (opt_cluster_cur_finished_nr)
            {
                cluster_cur_finished_nr = *opt_cluster_cur_finished_nr;
            }
            else if (!force_exit)
            {
                // timeouted.
                continue;
            }

            last_finished_nr = finished_nr;

            double cluster_ops = 1e9 * cluster_cur_finished_nr / ns;
            if (cluster_ops != 0)
            {
                perf_history.push_back(cluster_ops);
                perf_full_history.push_back(cluster_ops);
            }
            monitor_nr++;

            // for latency
            const auto &lat_m = proxy->core().global_latency_monitor();

            LOG(INFO) << "[master] ops: "
                      << util::pre_ops(cur_finished_nr, ns, true /* -v */)
                      << ", cluster: "
                      << util::pre_ops(cluster_cur_finished_nr, ns, true);
            // dont pring too much latency. Annoying
            if (!lat_m.empty() && monitor_nr % 3 == 0)
            {
                LOG(INFO) << "[master] latency: " << lat_m;
            }

            if (force_exit)
            {
                LOG_IF(INFO, force_exit)
                    << "[manager] Monitor exit: stop requested.";
                break;
            }
        }

        const auto &lat_m = proxy->core().global_latency_monitor();
        auto avg = perf_history.avg();
        auto diff = perf_history.diff();

        double variaty = 1.0 * diff / avg;
        proxy->core().request_stop();
        LOG_IF(WARNING, variaty >= 0.02)
            << "Large variaty detected. Diff " << diff << " of avg " << avg
            << " is " << util::pre_pcnt(variaty);
        LOG_IF(WARNING, avg == 0) << "** got result: " << avg;
        ResultLatencyRecord lat;
        if (!lat_m.empty())
        {
            lat.min = lat_m.min();
            auto p50 = lat_m.percentile(0.5);
            auto p90 = lat_m.percentile(0.9);
            auto p99 = lat_m.percentile(0.99);
            auto p999 = lat_m.percentile(0.999);

            lat.p50 = p50 ? *p50 : 0;
            lat.p90 = p90 ? *p90 : 0;
            lat.p99 = p99 ? *p99 : 0;
            lat.p999 = p999 ? *p999 : 0;
            lat.max = lat_m.max();
        }

        LOG_IF(INFO, report_history) << PRE(perf_full_history);

        return ResultRecord{
            .cluster_ops = avg, .variaty = variaty, .lat_ns = lat};
    }
};

}  // namespace bench::details