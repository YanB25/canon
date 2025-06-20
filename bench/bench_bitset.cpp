#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "DSM.h"
#include "DSMConfig.h"
#include "HugePageAlloc.h"
#include "WRLock.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Bitset.h"
#include "util/Numa.h"
#include "util/ProcessMem.h"
#include "util/RingBuffer.h"
#include "util/gflags_def.h"
#include "util/lock/RWLock.h"
#include "util/lock/TicketLock.h"
DEFINE_string(msg, "hello workd", "the message");

struct Spec
{
    struct TLS
    {
        std::unordered_set<uint64_t> set;
        size_t insert_nr = 0;
        size_t rm_nr = 0;
    };
};
constexpr static size_t kBitNr = 10_K;
inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::Spec::TLS &v)
{
    os << "{ThreadContext ";
    os << ", insert_nr: " << util::pre(v.insert_nr);
    os << ", rm_nr: " << util::pre(v.rm_nr);
    os << "}";
    return os;
}

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    using BLS = typename Base::BLS;
    Experiment() : bs(kBitNr)
    {
    }
    // the return idx satisfies
    // a) 0 <= idx < kBitNr
    // b) idx % kMaxAppThread == tid
    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        auto &tlc = bls.thread();
        // auto tid = util::get_thread_id();
        auto thread_nr = conf.thread_nr();
        while (likely(!token->stop_requested()))
        {
            auto idx = *bs.atomic_random_set();
            // LOG(INFO) << PRE(idx) << "(" << tid << ")";
            CHECK_EQ(tlc.set.count(idx), 0)
                << "got: " << idx << ", " << PRE(tlc.set);
            tlc.set.insert(idx);
            token->complete_task(1);
            tlc.insert_nr++;

            size_t kSizeLimit = (kBitNr / 2) / thread_nr;

            while (unlikely(tlc.set.size() >= kSizeLimit))
            {
                auto it = tlc.set.begin();
                auto idx = *it;
                tlc.set.erase(it);
                bs.reset(idx);
                // LOG(INFO) << "drop " << PRE(idx);
                tlc.rm_nr++;
                token->complete_task(1);
            }
        }
    }
    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        auto &tlc = bls.thread();
        std::lock_guard<std::mutex> lk(mu_);
        LOG(INFO) << PRE(tlc);
        for (auto idx : tlc.set)
        {
            CHECK_EQ(all_set.count(idx), 0);
            all_set.insert(idx);
        }
        Base::on_thread_end_bench(bls, conf);
    }
    void on_end_bench(const ::bench::ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        LOG(INFO) << "set bit number: " << all_set.size();
        Base::on_end_bench(res, bls, conf);
    }

private:
    util::synchronize::Bitset bs;
    std::mutex mu_;
    std::unordered_set<uint64_t> all_set;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;
    exp.configure_monitor(100ms, 50);

    ::bench::ConfigFactory f;
    f.configure_thread_nr({kMaxAppThread});

    exp.launch(f.generate_configs());

    LOG(INFO) << "PASS.";
}