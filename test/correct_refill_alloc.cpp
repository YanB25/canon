#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "DSMCache.h"
#include "WRLock.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/refill_allocator.h"
#include "util/gflags_def.h"
DEFINE_string(msg, "hello workd", "the message");

struct ThreadContext
{
    std::map<void *, size_t> set;
    std::shared_ptr<mem::LazySlabAllocator> alloc;
};

DEFINE_uint64(total_size, 1_GB, "total memory");
DEFINE_uint64(block_size, 2_MB, "block size");

struct Spec
{
    using TLS = ThreadContext;
};
class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    using S = ::bench::Storage<Spec>;
    Experiment()
    {
        g_alloc = std::make_shared<mem::BlockAllocator>(
            malloc(FLAGS_total_size), FLAGS_total_size, FLAGS_block_size);
    }
    void benchmark(S &store,
                   const Config &,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        auto &tlc = store.thread();

        while (likely(!token->stop_requested()))
        {
            auto size = fast_pseudo_rand_int(0, 1_KB);
            void *m = tlc.alloc->alloc(size);
            CHECK_EQ(tlc.set.count(m), 0);
            tlc.set.emplace(m, size);

            if (unlikely(tlc.set.size() >= 50))
            {
                auto it = tlc.set.begin();
                auto [m2, size2] = *it;
                tlc.set.erase(it);
                tlc.alloc->free(m2, size2);
            }

            token->complete_task(1);
        }
    }
    void on_thread_start_bench(S &store, const Config &conf) override
    {
        store.thread().alloc =
            std::make_shared<mem::LazySlabAllocator>(g_alloc, FLAGS_block_size);
        Base::on_thread_start_bench(store, conf);
    }
    void on_thread_end_bench(S &s, const Config &conf) override
    {
        auto &tlc = s.thread();
        {
            // LOG(INFO) << PRE(tlc.alloc->metrics());
            LOG(INFO) << PRE(tlc.alloc->upstream_alloc_nr());
            std::lock_guard<std::mutex> lk(mu_);
            for (auto idx : tlc.set)
            {
                CHECK_EQ(all_set.count(idx), 0);
                all_set.insert(idx);
            }
        }
        Base::on_thread_end_bench(s, conf);
    }
    void on_end_bench(const ::bench::ResultRecord &res,
                      S &s,
                      const Config &conf) override
    {
        // validate not overlapped
        for (auto it1 = all_set.begin(); it1 != all_set.end(); ++it1)
        {
            for (auto it2 = all_set.begin(); it2 != all_set.end(); ++it2)
            {
                if (it1 == it2)
                {
                    continue;
                }
                auto [m1, s1] = *it1;
                auto [m2, s2] = *it2;
                validate_buffer_not_overlapped(Buffer((char *) m1, s1),
                                               Buffer((char *) m2, s2));
            }
        }
        LOG(INFO) << "validate size: " << all_set.size();
        Base::on_end_bench(res, s, conf);
    }

private:
    mem::BlockAllocator::pointer g_alloc;
    std::mutex mu_;
    std::set<std::pair<void *, size_t>> all_set;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    f.configure_thread_nr({kMaxAppThread});
    auto configs = f.generate_configs();

    exp.configure_monitor(100ms, 30);
    exp.launch(configs);

    LOG(INFO) << "PASS.";
}