#pragma once
#include <utility>

#include "bench/experiment.h"
#include "config_factory.h"

namespace bench
{
template <typename Spec, typename ConfigT = QuickBenchConfig>
class LocalExperiment : public IExperiment<Spec, ConfigT>
{
public:
    using Config = ConfigT;
    using Base = IExperiment<Spec, Config>;
    using StorageT = typename Base::StorageT;
    using BLS = StorageT;
    void cluster_barrier(const std::string &) override
    {
    }
    std::optional<uint64_t> cluster_sum(uint64_t val,
                                        std::chrono::nanoseconds) override
    {
        return val;
    }
    void on_end_bench(const ::bench::ResultRecord &results,
                      StorageT &store,
                      const Config &conf) override
    {
        LOG(INFO) << PRE(results);
        Base::on_end_bench(results, store, conf);
    }
    void on_start_bench(StorageT &store, const Config &conf) override
    {
        LOG(INFO) << PRE(conf);
        Base::on_start_bench(store, conf);
    }
    virtual ~LocalExperiment() = default;

private:
};

}  // namespace bench