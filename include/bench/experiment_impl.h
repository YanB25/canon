#pragma once

#include "bench/coro_manager.h"
#include "bench/experiment.h"
#include "bench/manager.h"

namespace bench
{
template <typename Spec, typename Config>
void IExperiment<Spec, Config>::launch(
    const std::vector<std::shared_ptr<IBenchConfig>> &configs)
{
    size_t max_thread_nr = 1;
    for (const auto &conf : configs)
    {
        max_thread_nr = std::max(max_thread_nr, conf->thread_nr());
    }
    auto manager =
        details::Manager<Spec, Config, IExperiment<Spec, Config>>::new_instance(
            max_thread_nr, *this);
    manager->set_enable_monitor(enable_monitor_);
    manager->register_default_monitor(
        std::chrono::nanoseconds(monitor_ns_), monitor_epoch_, report_history_);
    manager->bench(configs);
    manager.reset();  // dctor here
}

template <typename Spec, typename Config>
void IExperiment<Spec, Config>::launch_coroutines(
    const std::vector<std::shared_ptr<IBenchConfig>> &configs)
{
    size_t max_thread_nr = 1;
    size_t max_coro_nr = 1;
    for (const auto &conf : configs)
    {
        max_thread_nr = std::max(max_thread_nr, conf->thread_nr());
        max_coro_nr = std::max(max_coro_nr, conf->coro_nr());
    }
    CHECK_LE(max_thread_nr, kMaxAppThread);
    CHECK_LE(max_coro_nr, define::kMaxCoroNr);

    auto manager =
        details::CoroManager<Spec, Config, IExperiment<Spec, Config>>::
            new_instance(max_thread_nr, max_coro_nr, *this);
    manager->register_default_monitor(
        std::chrono::nanoseconds(monitor_ns_), monitor_epoch_, report_history_);
    manager->bench(configs);

    manager.reset();  // dctor here
}
}  // namespace bench