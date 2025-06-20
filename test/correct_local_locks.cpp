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
#include "util/lock/Guard.h"
#include "util/lock/MCSLock.h"
#include "util/lock/RWLock.h"
#include "util/lock/ReaderPriorityLock.h"
#include "util/lock/TicketLock.h"

struct Spec
{
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    Experiment() : a(0), b(0)
    {
    }
    template <typename Lock>
    void do_benchmark(Lock &lock,
                      TLS &,
                      const Config &conf,
                      ::bench::StopToken::pointer token)
    {
        auto tid = util::get_thread_id();
        size_t thread_nr = conf.thread_nr();
        size_t reader_nr =
            thread_nr * (conf.get<uint64_t>("reader_rate") / 100.0);
        while (likely(!token->stop_requested()))
        {
            uint64_t val = fast_pseudo_rand_int();
            if (tid < reader_nr)
            {
                // readers
                util::SharedGuard guard(lock);
                uint64_t read_a = a.load(std::memory_order_relaxed);
                uint64_t read_b = b.load(std::memory_order_relaxed);
                CHECK_EQ(read_a, read_b);
            }
            else
            {
                // writers
                util::UniqueGuard guard(lock);
                a.store(val, std::memory_order_relaxed);
                b.store(val, std::memory_order_relaxed);
            }
            token->complete_task(1);
        }
    }
    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        auto &tlc = bls.thread();
        std::string lock_name = conf.get<const char *>("lock");

        if (lock_name == "rw")
        {
            do_benchmark<util::RWLock>(rw_lock, tlc, conf, token);
        }
        else if (lock_name == "ticket")
        {
            do_benchmark<util::TicketLock>(ticket_lock, tlc, conf, token);
        }
        else if (lock_name == "mcs")
        {
            do_benchmark<util::MCSLock>(mcs_lock, tlc, conf, token);
        }
        else if (lock_name == "rp")
        {
            do_benchmark<util::RPLock>(rp_lock, tlc, conf, token);
        }
        else
        {
            LOG(FATAL) << "Unknown lock name " << PRE(lock_name);
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
    util::RWLock rw_lock;
    util::MCSLock mcs_lock;
    util::TicketLock ticket_lock;
    util::RPLock rp_lock;
    std::atomic<uint64_t> a;
    std::atomic<uint64_t> b;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    ::bench::ConfigFactory f;
    // f.configure_thread_nr({1, 4, 16, 32});
    f.configure_thread_nr({1, 4});
    f.add_option_df("reader_rate", {0, 50, 100});
    f.add_option_df("lock", {"rw", "ticket", "mcs", "rp"});
    // f.add_option_df("lock", {"rp"});

    Experiment exp;
    exp.configure_monitor(200ms, 5);
    exp.launch(f.generate_configs());

    LOG(INFO) << "PASS.";
}