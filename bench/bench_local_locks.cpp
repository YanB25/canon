#include <numa.h>

#include <thread>

#include "DSM.h"
#include "DSMConfig.h"
#include "HugePageAlloc.h"
#include "WRLock.h"
#include "bench/DataFrame.h"
#include "bench/base_config.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Numa.h"
#include "util/ProcessMem.h"
#include "util/ThreadSafeHashCache.h"
#include "util/ThreadSafeSkipList.h"
#include "util/gflags_def.h"
#include "util/lock/MCSLock.h"
#include "util/lock/RWLock.h"
#include "util/lock/ReaderPriorityLock.h"
#include "util/lock/TicketLock.h"

struct Spec
{
    struct TLS
    {
        size_t read_nr{0};
        size_t write_nr{0};
    };
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec, Config>;
    using BLS = typename Base::BLS;
    using TLS = typename BLS::TLS;
    template <typename Lock>
    void do_benchmark(Lock &lock,
                      TLS &tlc,
                      const Config &conf,
                      ::bench::StopToken::pointer token)
    {
        auto writer_nr = conf.get<size_t>("writer_nr");
        auto writer_wait_ns = conf.get<size_t>("writer_wait_ns");
        auto reader_wait_ns = conf.get<size_t>("reader_wait_ns");
        while (likely(!token->stop_requested()))
        {
            auto tid = util::get_thread_id();
            if (tid < writer_nr)
            {
                // writer
                if (writer_wait_ns > 0)
                {
                    std::this_thread::sleep_for(
                        std::chrono::nanoseconds(writer_wait_ns));
                }
                lock.write_lock();
                lock.write_unlock();
                tlc.write_nr++;
            }
            else
            {
                if (reader_wait_ns > 0)
                {
                    std::this_thread::sleep_for(
                        std::chrono::nanoseconds(reader_wait_ns));
                }
                lock.read_lock();
                lock.read_unlock();
                tlc.read_nr++;
            }
            token->complete_task(1);
        }
    }
    void benchmark(BLS &bls,
                   const Config &config,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        auto &tlc = bls.thread();
        auto lock_name = config.get<std::string>("lock_name");
        if (lock_name == "rw")
        {
            do_benchmark<util::RWLock>(rw_lock, tlc, config, token);
        }
        else if (lock_name == "ticket")
        {
            do_benchmark<util::TicketLock>(ticket_lock, tlc, config, token);
        }
        else if (lock_name == "mcs")
        {
            do_benchmark<util::MCSLock>(mcs_lock, tlc, config, token);
        }
        else if (lock_name == "rp")
        {
            do_benchmark<util::RPLock>(rp_lock, tlc, config, token);
        }
        else
        {
            LOG(FATAL) << "Unknown lock name " << PRE(lock_name);
        }
    }
    void on_end_bench(const ::bench::ResultRecord &results,
                      BLS &bls,
                      const Config &conf) override
    {
        LOG(INFO) << PRE(results);
        auto writer_nr = conf.get<size_t>("writer_nr");
        df.reg_result(results, conf);
        df.reg_result("writer_nr", writer_nr);
        Base::on_end_bench(results, bls, conf);
    }
    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        auto &tlc = bls.thread();
        tlc.read_nr = 0;
        tlc.write_nr = 0;
        Base::on_thread_end_bench(bls, conf);
    }
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        LOG(INFO) << PRE(conf);
        Base::on_start_bench(bls, conf);
    }
    void exit() override
    {
        df.dump(FLAGS_binary, FLAGS_exec_meta);
        Base::exit();
    }

private:
    util::RWLock rw_lock;
    util::TicketLock ticket_lock;
    util::MCSLock mcs_lock;
    util::RPLock rp_lock;

    std::vector<uint64_t> col_reader_nr;
    bench::ResultDataFrame df;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    Experiment exp;
    exp.configure_monitor(100ms, 10);
    std::vector<::bench::IBenchConfig::Pointer> configs;

    bench::ConfigFactory f;
    f.configure_thread_nr({2, 4, 8, 16, 24, kMaxAppThread});
    // f.add_option<std::string>("lock_name", {"rw", "ticket", "mcs", "rp"});
    f.add_option<std::string>("lock_name", {"rp"});
    f.add_option<size_t>("writer_nr", {1});
    f.add_option<size_t>("writer_wait_ns", {100});
    // f.add_option<size_t>("writer_wait_ns", {0});
    f.add_option<size_t>("reader_wait_ns", {0});

    exp.launch(f.generate_configs());

    LOG(INFO) << "PASS.";
}