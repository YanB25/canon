#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <queue>
#include <set>

#include "Common.h"
#include "PerThread.h"
#include "Rdma.h"
#include "bench/DataFrame.h"
#include "bench/experiment_impl.h"
#include "bench/manager.h"
#include "boost/thread/barrier.hpp"
#include "dsm_experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "sherman/Tree.h"
#include "util/CLog.h"
#include "util/DataFrameF.h"
#include "util/PerformanceReporter.h"
#include "util/Rand.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"

using namespace util::literals;
using namespace std::chrono_literals;
using namespace sherman;
using Tree = sherman::Tree;

using IBenchConfig = ::bench::IBenchConfig;

struct Spec
{
    struct TLS
    {
        size_t op;
    };
};

struct LogEntry
{
    uint32_t tid;
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    using BLS = typename Base::BLS;
    Experiment()
    {
    }

    void benchmark(BLS &,
                   const Config &config,
                   ::bench::StopToken::pointer token,
                   bool is_master) override
    {
        std::ignore = config;
        std::ignore = is_master;
        auto tid = util::get_thread_id();
        LogEntry le;
        le.tid = tid;
        while (!token->stop_requested())
        {
            if (clog.append(le))
            {
                token->complete_task(1);
            }
        }
    }
    void on_end_bench(const ::bench::ResultRecord &result,
                      BLS &bls,
                      const Config &conf) override
    {
        df.reg_result(result, conf);
        // LOG(INFO) << clog;
        Base::on_end_bench(result, bls, conf);
    }

    void exit() override
    {
        df.dump(FLAGS_binary, FLAGS_exec_meta);
        Base::exit();
    }

private:
    bench::ResultDataFrame df;
    util::synchronize::CLog<LogEntry, 102400> clog;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    Experiment exp;
    exp.configure_monitor(100ms, 10);
    ::bench::ConfigFactory f;
    // f.configure_thread_nr({1, 2, 4, 8, 16, 24});
    f.configure_thread_nr({1, 2, 4, 16, 24});

    exp.launch(f.generate_configs());

    // {
    //     util::synchronize::CLog<LogEntry, 16> clog;
    //     LogEntry le;
    //     le.tid = 62;
    //     CHECK(clog.append(le));
    //     LOG(INFO) << clog;
    // }
}
