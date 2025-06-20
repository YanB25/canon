#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Hexdump.hpp"
#include "util/gflags_def.h"

struct ThreadContext
{
};

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
        DSMConfig config;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();
        gaddr_ = dsm_->alloc(kInternalPageSize);
    }
    void thread_init(BLS &bls, size_t thread_nr) override
    {
        dsm_->registerThread();
        Base::thread_init(bls, thread_nr);
    }
    void checker(BLS &bls,
                 const Config &conf,
                 ::bench::StopToken::pointer token)
    {
        std::ignore = bls;
        std::ignore = conf;

        auto rdma_buf = dsm_->get_rdma_buffer(kInternalPageSize);
        while (!token->stop_requested())
        {
            dsm_->read_sync(rdma_buf.buffer, gaddr_, kInternalPageSize);
            check_consistent(rdma_buf.buffer, kInternalPageSize);
        }
    }
    void check_consistent(const char *buffer, size_t len)
    {
        char first = buffer[0];
        char last = buffer[len - 1];
        std::optional<size_t> first_change_idx;
        for (size_t i = 0; i < len; ++i)
        {
            if (first == last)
            {
                CHECK_EQ(first, buffer[i]) << util::Hexdump(buffer, len);
                // LOG_FIRST_N(INFO, 10) << std::endl
                //                       << util::Hexdump(buffer, len);
            }
            else
            {
                // LOG_FIRST_N(INFO, 10) << std::endl
                //                       << util::Hexdump(buffer, len);

                if (buffer[i] != first && buffer[i] != last)
                {
                    LOG(FATAL)
                        << "Buffer into (more than) three pieces: " << std::endl
                        << util::Hexdump(buffer, len);
                }
                if (buffer[i] == last)
                {
                    if (!first_change_idx)
                    {
                        first_change_idx = i;
                    }
                    else
                    {
                        CHECK_GT(i, *first_change_idx)
                            << std::endl
                            << util::Hexdump(buffer, len);
                    }
                }
                if (buffer[i] == first)
                {
                    CHECK(!first_change_idx.has_value())
                        << PRE(first_change_idx.value()) << std::endl
                        << util::Hexdump(buffer, len);
                }
            }
        }
    }
    void issuer(BLS &bls, const Config &conf, ::bench::StopToken::pointer token)
    {
        std::ignore = bls;
        std::ignore = conf;
        auto rdma_buf = dsm_->get_rdma_buffer(kInternalPageSize);
        while (!token->stop_requested())
        {
            char ch = fast_pseudo_rand_printable_char();
            memset(rdma_buf.buffer, ch, kInternalPageSize);
            dsm_->write_sync(rdma_buf.buffer, gaddr_, kInternalPageSize);
            token->complete_task(1);
        }
    }

    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        auto tid = util::get_thread_id();
        if (tid == 0)
        {
            checker(bls, conf, token);
        }
        else
        {
            issuer(bls, conf, token);
        }
    }
    void on_thread_start_bench(BLS &bls, const Config &conf) override
    {
        Base::on_thread_start_bench(bls, conf);
    }

private:
    DSM::pointer dsm_;
    GlobalAddress gaddr_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    f.configure_thread_nr({2});
    auto configs = f.generate_configs();

    exp.configure_monitor(1s, 10);
    exp.launch(configs);

    LOG(INFO) << "PASS.";
}