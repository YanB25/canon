#include <numa.h>

#include <thread>

#include "DSM.h"
#include "DSMConfig.h"
#include "HugePageAlloc.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Hexdump.hpp"
#include "util/Numa.h"
#include "util/ProcessMem.h"
#include "util/RingBuffer.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"
#include "util/lock/Guard.h"
#include "util/lock/MCSLock.h"
#include "util/lock/RWLock.h"
#include "util/lock/TicketLock.h"

struct Spec
{
};

struct Cacheline
{
    std::atomic<uint32_t> f_version{0};
    std::shared_ptr<util::Page> data;
    std::atomic<uint32_t> t_version{0};
} __attribute__((packed));
static_assert(sizeof(Cacheline) <= 64);

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    Experiment()
    {
        util::Page page(64);
        for (size_t i = 0; i < 64; ++i)
        {
            page.data()[i] = 'a';
        }
        cl_.data = std::make_shared<util::Page>(std::move(page));
    }
    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        // auto &tlc = bls.thread();
        std::ignore = bls;
        std::ignore = conf;
        auto tid = util::get_thread_id();

        if (tid == 0)
        {
            // writer
            while (!token->stop_requested())
            {
                cl_.f_version.fetch_add(1);
                char new_char = 'a' + fast_pseudo_rand_int(0, 26);
                {
                    auto ptr = cl_.data;
                    auto new_page = *ptr;
                    for (size_t i = 0; i < ptr->size(); ++i)
                    {
                        std::atomic<char> *atm =
                            (std::atomic<char> *) &(new_page.data()[i]);
                        // ptr->data()[i] = new_char;
                        atm->store(new_char);
                    }
                    cl_.data =
                        std::make_shared<util::Page>(std::move(new_page));
                }
                cl_.t_version.fetch_add(1);
                token->complete_task(1);
            }
        }
        else
        {
            // reader
            while (!token->stop_requested())
            {
                auto f = cl_.f_version.load();
                auto ptr = cl_.data;
                auto t = cl_.t_version.load();
                if (f == t)
                {
                    check_consistent(ptr, f, t);
                    token->complete_task(1);
                }
                token->complete_task(1);
            }
        }
    }

    void check_consistent(std::shared_ptr<util::Page> page_ptr,
                          uint64_t f,
                          uint64_t e)
    {
        if (!page_ptr)
        {
            return;
        }
        std::atomic<char> *first = (std::atomic<char> *) &(page_ptr->data()[0]);
        char first_ch = first->load();
        for (size_t i = 0; i < page_ptr->size(); ++i)
        {
            std::atomic<char> *cur =
                (std::atomic<char> *) &(page_ptr->data()[i]);
            char cur_ch = cur->load();
            CHECK_EQ(cur_ch, first_ch)
                << PRE(f) << ", " << PRE(e) << std::endl
                << util::Hexdump(page_ptr->data(), page_ptr->size());
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
    Cacheline cl_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    ::bench::ConfigFactory f;
    f.configure_thread_nr({1, 4, 8, 16, 32});

    Experiment exp;
    exp.configure_monitor(200ms, 5);
    exp.launch(f.generate_configs());

    LOG(INFO) << "PASS.";
}