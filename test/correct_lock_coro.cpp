#include <numa.h>

#include <thread>

#include "DSM.h"
#include "DSMConfig.h"
#include "HugePageAlloc.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Numa.h"
#include "util/ProcessMem.h"
#include "util/RingBuffer.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"
#include "util/lock/MCSLock.h"
#include "util/lock/RWLock.h"
#include "util/lock/TicketLock.h"

struct Spec
{
    struct GLS
    {
        util::TicketLockManager ticket_lock{1};
        std::atomic<uint64_t> a{0};
        std::atomic<uint64_t> b{0};
    };
    struct TLS
    {
        std::queue<int> hot_waiting_queue;
        std::vector<bool> coro_finished;
        size_t finished_nr{0};
    };
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;

    void benchmark_worker(BLS &bls,
                          const Config &,
                          CoroContext &ctx,
                          ::bench::StopToken::pointer token,
                          bool) override
    {
        auto &gls = bls.global();
        auto &tls = bls.thread();
        auto &lock = gls.ticket_lock;
        auto &queue = bls.thread().hot_waiting_queue;

        while (!token->stop_requested())
        {
            uint64_t val = fast_pseudo_rand_int();

            lock.acquire(0, &ctx, &queue);

            uint64_t a = gls.a.load(std::memory_order_relaxed);
            uint64_t b = gls.b.load(std::memory_order_relaxed);
            gls.a.store(val, std::memory_order_relaxed);
            gls.b.store(val, std::memory_order_relaxed);

            lock.release(0, &ctx);

            CHECK_EQ(a, b) << "** lock semantics violated";

            token->complete_task(1);
        }

        tls.coro_finished[ctx.coro_id()] = true;
        tls.finished_nr++;

        ctx.record_yield_reason(true /* exit */);
        ctx.yield_to_master();
        LOG(FATAL);
    }

    void benchmark_master(BLS &bls,
                          const Config &conf,
                          CoroContext &ctx,
                          ::bench::StopToken::pointer,
                          bool) override
    {
        auto &tls = bls.thread();
        size_t coro_nr = conf.coro_nr();
        tls.coro_finished.resize(coro_nr, false);
        tls.finished_nr = 0;

        for (size_t i = 0; i < coro_nr; ++i)
        {
            ctx.yield_to_worker(i);
        }

        while (tls.finished_nr < coro_nr)
        {
            // for (size_t i = 0; i < coro_nr; ++i)
            // {
            //     if (!tls.coro_finished[i])
            //     {
            //         LOG(INFO) << util::get_thread_id() << " to " << i;
            //         ctx.yield_to_worker(i);
            //     }
            // }
            while (!tls.hot_waiting_queue.empty())
            {
                auto coro = tls.hot_waiting_queue.front();
                tls.hot_waiting_queue.pop();
                ctx.yield_to_worker(coro);
            }
        }
    }

    void on_start_bench(BLS &bls, const Config &conf) override
    {
        LOG(INFO) << PRE(conf);
        Base::on_start_bench(bls, conf);
    }

    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        Base::on_thread_end_bench(bls, conf);
    }
    void on_end_bench(const ::bench::ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        Base::on_end_bench(res, bls, conf);
    }

private:
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    ::bench::ConfigFactory f;

    f.configure_thread_nr({1, 16, kMaxAppThread});
    f.configure_coro_nr({1, 4, 8});

    Experiment exp;
    exp.configure_monitor(200ms, 8);
    exp.launch_coroutines(f.generate_configs());

    LOG(INFO) << "PASS.";
}