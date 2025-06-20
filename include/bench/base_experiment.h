#pragma once
#include <chrono>
#include <ratio>

#include "CoroContext.h"
#include "base_config.h"
#include "bench/DataFrame.h"
#include "result.h"
#include "storage.h"
#include "token.h"
#include "util/TimeConv.h"

namespace bench
{
template <typename StorageSpec, typename Config>
class IExperiment
{
public:
    using GLS = spec_gls_t<StorageSpec>;
    using TLS = spec_tls_t<StorageSpec>;
    using CLS = spec_cls_t<StorageSpec>;
    using StorageT = Storage<StorageSpec>;
    using BLS = StorageT;
    using ResultRecord = bench::ResultRecord;

    virtual void init()
    {
    }
    virtual void thread_init(BLS &, [[maybe_unused]] size_t thread_nr)
    {
    }
    virtual void coro_init(BLS &, [[maybe_unused]] size_t coro_nr)
    {
    }

    virtual std::optional<uint64_t> cluster_sum(uint64_t,
                                                std::chrono::nanoseconds)
    {
        LOG(FATAL) << "** no cluster_sum provided";
    }
    virtual void cluster_barrier(const std::string &)
    {
        LOG(FATAL) << "** no cluster barrier provided";
    }
    // node_id and cluster size
    virtual int node_id()
    {
        return -1;  // unknown
    }

    virtual void on_start_bench(StorageT &, const Config &)
    {
    }
    virtual void on_thread_start_bench(StorageT &, const Config &)
    {
    }
    virtual void on_coro_start_bench(StorageT &, const Config &)
    {
    }
    virtual void on_coro_end_bench(StorageT &, const Config &)
    {
    }
    virtual void on_thread_end_bench(StorageT &, const Config &)
    {
    }
    virtual void on_end_bench(const ResultRecord &res,
                              StorageT &,
                              const Config &conf)
    {
        df_.reg_result(res, conf);
    }

    auto &df()
    {
        return df_;
    }

    virtual void exit()
    {
        df_.dump(FLAGS_binary, FLAGS_exec_meta);
    }

    template <typename A, typename B>
    void configure_monitor(std::chrono::duration<A, B> dur, size_t epoch)
    {
        monitor_ns_ =
            std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
        monitor_epoch_ = epoch;
    }
    void configure_report_history(bool en)
    {
        report_history_ = en;
    }

    // For no coroutine: implement these
    virtual void benchmark(StorageT &, const Config &, StopToken::pointer, bool)
    {
        LOG(FATAL) << "** no benchmark registered";
    }

    virtual void launch(
        const std::vector<std::shared_ptr<::bench::IBenchConfig>> &configs);

    // For coroutines: implement these
    virtual void benchmark_worker(StorageT &,
                                  const Config &,
                                  CoroContext &,
                                  ::bench::StopToken::pointer,
                                  bool)
    {
        LOG(FATAL) << "** no benchmark_worker registered";
    }
    virtual void benchmark_master(StorageT &,
                                  const Config &,
                                  CoroContext &,
                                  ::bench::StopToken::pointer,
                                  bool)
    {
        LOG(FATAL) << "** no benchmark_master registered";
    }
    void launch_coroutines(
        const std::vector<std::shared_ptr<IBenchConfig>> &configs);
    virtual ~IExperiment() = default;

    void disable_monitor()
    {
        enable_monitor_ = false;
    }

private:
    uint64_t monitor_ns_{util::time::to_ns(1s)};
    uint64_t monitor_epoch_{10};

    bool enable_monitor_{true};
    bool report_history_{false};

    bench::ResultDataFrame df_;
};

}  // namespace bench