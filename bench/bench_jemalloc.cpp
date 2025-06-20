#include <numa.h>

#include <thread>

#include "PerThread.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "jemalloc_cpp/jemalloc_cpp.h"
#include "patronus/memory/direct_allocator.h"
#include "util/LRU.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"

using Jemalloc::Tag;

struct Spec
{
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    using BLS = typename Base::BLS;

    Experiment()
    {
        m_ = std::make_shared<patronus::mem::MallocAllocator>();
        // prepare jemalloc
        Jemalloc::prepare_allocator<Tag::Test>(
            Jemalloc::JemallocParam<Tag::Test>{m_.get()});
        // prepare my slab
    }
    void on_thread_start_bench(BLS &bls, const Config &conf) override
    {
        je_alloc_.current() =
            Jemalloc::JemallocAllocator<Tag::Test>::make_allocator();
        slabs_.current() = std::make_shared<mem::LazySlabAllocator>(m_, 2_MB);
        Base::on_thread_start_bench(bls, conf);
    }
    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        std::ignore = bls;
        std::ignore = conf;
        bool use_je = conf.get<bool>("use_je");
        bool rand = conf.get<bool>("rand");
        while (!token->stop_requested())
        {
            auto size = 64;
            if (rand)
            {
                size = fast_pseudo_rand_int(1, 4_KB);
            }
            if (use_je)
            {
                [[maybe_unused]] auto *addr = je_alloc_.current()->alloc(size);
                // je_alloc_.current()->free(addr, size);
            }
            else
            {
                [[maybe_unused]] auto *addr = slabs_.current()->alloc(size);
                // slabs_.current()->free(addr, size);
            }

            token->complete_task(1);
        }
    }

private:
    Perthread<std::shared_ptr<Jemalloc::JemallocAllocator<Tag::Test>>>
        je_alloc_;
    Perthread<std::shared_ptr<mem::LazySlabAllocator>> slabs_;

    std::shared_ptr<patronus::mem::MallocAllocator> m_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    Experiment exp;
    exp.configure_monitor(100ms, 10);
    ::bench::ConfigFactory f;

    f.configure_thread_nr({kMaxAppThread});
    f.add_option<bool>("use_je", {true, false});
    f.add_option<bool>("rand", {true, false});

    exp.launch(f.generate_configs());

    LOG(INFO) << "PASS.";
}