#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "DSMCache.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/gflags_def.h"

struct Spec
{
};

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using BLS = typename Base::BLS;
    Experiment()
    {
        DSMConfig dsm_config;
        dsm_config.worker_nr = 0;
        dsm_ = DSM::getInstance(dsm_config);
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }

    void thread_init(BLS &bls, size_t thread_nr) override
    {
        dsm_->registerThread();

        Base::thread_init(bls, thread_nr);
    }
    void benchmark_raw(BLS &,
                       const Config &conf,
                       ::bench::StopToken::pointer token)
    {
        auto max_queue_depth = conf.coro_nr();
        auto signal_every = conf.get<size_t>("signal_nr");
        auto io_size = conf.get<size_t>("io_size");
        auto dsm_range = conf.get<size_t>("io_rng");

        auto to_node = (dsm_->get_node_id() + 1) % dsm_->getClusterSize();
        auto dir_id = 0;

        auto lkey = dsm_->get_icon_lkey();
        auto rkey = dsm_->get_rkey(to_node, dir_id);
        GlobalAddress gaddr;
        gaddr.nodeID = to_node;
        auto dsm_base = dsm_->remote_info()[to_node].dsmBase;

        size_t credit = max_queue_depth;
        auto &qp = dsm_->get_th_cqp(to_node, dir_id);

        ibv_exp_wc wcs[64];

        auto *ibqp = qp.ibqp();
        auto *ibcq = qp.ibcq();

        auto *local_rdma_buf = (char *) dsm_->get_cache().data;
        local_rdma_buf = util::to_aligned(local_rdma_buf, io_size);
        size_t ongoing_signal = 0;

        while (!token->stop_requested())
        {
            for (size_t i = 0; i < credit; ++i)
            {
                ibv_exp_send_wr wrs[16];
                ibv_sge sges[16];
                for (size_t s = 0; s < signal_every; ++s)
                {
                    // Context *context = (Context *) jemalloc(sizeof(Context));
                    // context->rdma_buf = dsm_->get_rdma_buffer(io_size);
                    gaddr.offset =
                        io_size * fast_pseudo_rand_int(0, dsm_range / io_size);
                    uint64_t dest = gaddr.offset + dsm_base;
                    bool last = s + 1 == signal_every;
                    bool signal = last;
                    auto &wr = wrs[s];
                    auto &sge = sges[s];
                    uint64_t source =
                        (uint64_t) local_rdma_buf +
                        io_size * fast_pseudo_rand_int(0, 2_MB / io_size);
                    fillSgeWr(sge, wr, source, io_size, lkey);
                    wr.exp_opcode = IBV_EXP_WR_RDMA_READ;
                    wr.exp_send_flags = 0;
                    if (signal)
                    {
                        wr.exp_send_flags |= IBV_EXP_SEND_SIGNALED;
                    }
                    wr.wr.rdma.remote_addr = dest;
                    wr.wr.rdma.rkey = rkey;
                    // wr.wr_id = (uint64_t) context;
                    wr.wr_id = 0;

                    token->complete_task(1);
                }
                for (size_t s = 0; s < signal_every; ++s)
                {
                    bool last = s + 1 == signal_every;
                    if (!last)
                    {
                        wrs[s].next = &wrs[s + 1];
                    }
                    else
                    {
                        wrs[s].next = nullptr;
                    }
                }
                ibv_exp_send_wr *bad_wr;
                int ret = ibv_exp_post_send(ibqp, wrs, &bad_wr);
                PLOG_IF(FATAL, ret < 0)
                    << "** failed to ibv_exp_post_send: " << PRE(*bad_wr);
            }
            ongoing_signal += credit;
            credit = 0;

            int ret = ibv_exp_poll_cq(ibcq, 64, wcs, sizeof(ibv_exp_wc));
            PLOG_IF(FATAL, ret < 0) << "** failed to ibv_exp_poll_cq";
            if (ret)
            {
                credit += ret;
                ongoing_signal -= ret;
            }
        }
        LOG(INFO) << "joining...";
        while (ongoing_signal)
        {
            ibv_exp_wc wc;
            int ret = ibv_exp_poll_cq(ibcq, 1, &wc, sizeof(wc));
            PLOG_IF(FATAL, ret < 0) << "** failed to ibv_exp_poll_cq";
            ongoing_signal -= ret;
        }
    }
    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        bool raw = conf.get<bool>("raw");
        if (raw)
        {
            benchmark_raw(bls, conf, token);
        }
        else
        {
            benchmark_regular(bls, conf, ctx, token);
        }
    }
    void benchmark_regular(BLS &,
                           const Config &conf,
                           CoroContext &ctx,
                           ::bench::StopToken::pointer token)
    {
        auto signal_nr = conf.get<size_t>("signal_nr");
        auto io_size = conf.get<size_t>("io_size");
        auto io_rng = conf.get<size_t>("io_rng");

        std::vector<Buffer> rdma_bufs;
        GlobalAddress gaddr;
        gaddr.nodeID = (dsm_->get_node_id() + 1) % dsm_->getClusterSize();

        while (!token->stop_requested())
        {
            for (size_t i = 0; i < signal_nr; ++i)
            {
                gaddr.offset =
                    fast_pseudo_rand_int(0, io_rng / io_size) * io_size;
                rdma_bufs.emplace_back(dsm_->get_rdma_buffer(io_size));
                dsm_->prepare_read(
                    rdma_bufs.back().buffer, gaddr, io_size, false, &ctx);
                token->complete_task(1);
            }
            dsm_->commit(&ctx);

            for (auto &&buf : rdma_bufs)
            {
                dsm_->put_rdma_buffer(std::move(buf));
            }
            rdma_bufs.clear();
        }
    }

private:
    DSM::pointer dsm_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    f.configure_thread_nr({1});
    f.configure_coro_nr({16});
    // f.add_option<bool>("raw", {true, false});
    // f.add_option<bool>("raw", {true});
    f.add_option<bool>("raw", {false});
    f.add_option<size_t>("signal_nr", {6});
    f.add_option<size_t>("io_size", {8});
    f.add_option<size_t>("io_rng", {1_GB});

    auto configs = f.generate_configs();

    exp.configure_monitor(200ms, 20);
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}