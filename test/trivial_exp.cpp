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
        Jemalloc::prepare_allocator<Tag::Test>(&m_);
    }
    void on_thread_start_bench(BLS &bls, const Config &conf) override
    {
        alloc_.current() =
            Jemalloc::JemallocAllocator<Tag::Test>::make_allocator();
        Base::on_thread_start_bench(bls, conf);
    }
    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        std::ignore = bls;
        std::ignore = conf;
        std::ignore = token;
        std::vector<uint64_t> vec;
        for (size_t i = 0; i < 10_K; ++i)
        {
            auto *addr = alloc_.current()->alloc(64);
            addrs.current().push_back(Buffer((char *) addr, 64));
        }
        // LOG(INFO) << bls.id().thread_id << ": " << PRE(addrs.current());
    }
    void on_end_bench(const ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        std::vector<Buffer> gvec;
        for (auto &vec : addrs)
        {
            gvec.insert(gvec.end(),
                        std::make_move_iterator(vec.get().begin()),
                        std::make_move_iterator(vec.get().end()));
        }
        validate_buffer_not_overlapped(gvec);

        Base::on_end_bench(res, bls, conf);
    }

private:
    Perthread<std::vector<Buffer>> addrs;
    Perthread<std::shared_ptr<Jemalloc::JemallocAllocator<Tag::Test>>> alloc_;

    patronus::mem::MallocAllocator m_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    Experiment exp;
    exp.configure_monitor(100ms, 10);
    ::bench::ConfigFactory f;

    f.configure_thread_nr({8});

    exp.launch(f.generate_configs());

    LOG(INFO) << "PASS.";
}