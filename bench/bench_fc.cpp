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
#include "dsm_experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/DataFrameF.h"
#include "util/FC.h"
#include "util/PerformanceReporter.h"
#include "util/gflags_def.h"

using namespace util::literals;
using namespace std::chrono_literals;

using IBenchConfig = ::bench::IBenchConfig;

struct Spec
{
};

struct LogEntry
{
    uint32_t tid;
};

struct QRequest : public util::synchronize::IRequest
{
    size_t tid;
    bool is_enq;
};

struct QResponse : public util::synchronize::IResponse
{
    bool succ;
    size_t val;
};

std::ostream &operator<<(std::ostream &os, const QResponse &resp)
{
    os << "{Succ: " << resp.succ << ", " << resp.val << "}";
    return os;
}

struct ExecF
{
    QResponse operator()(std::queue<size_t> *state, const QRequest &req)
    {
        QResponse ret;
        if (req.is_enq)
        {
            state->push(req.tid);
            ret.succ = true;
        }
        else
        {
            if (state->empty())
            {
                ret.succ = false;
            }
            else
            {
                ret.val = state->front();
                state->pop();
                ret.succ = true;
            }
        }
        return ret;
    }
};

using FC = util::synchronize::
    FlatCombining<std::queue<size_t>, QRequest, QResponse, ExecF>;

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

        QRequest req;
        req.set_valid();
        req.tid = tid;
        while (!token->stop_requested())
        {
            if (fast_pseudo_bool_with_prob(0.8))
            {
                req.is_enq = true;
            }
            else
            {
                req.is_enq = false;
            }
            [[maybe_unused]] auto &my_resp = fc_.execute(req);
            token->complete_task(1);
            // LOG_EVERY_N(INFO, 100000)
            //     << "get succ " << my_resp.succ << " at " << (void *)
            //     &my_resp;
            // LOG_EVERY_N(INFO, 100000) << my_resp << " from " << tid;
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
    FC fc_;
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
