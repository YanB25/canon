#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "DSMCache.h"
#include "RPC.h"
#include "WRLock.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
#include "util/gflags_def.h"

using namespace rpc;

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
        DSMConfig config;
        dsm_ = DSM::getInstance(config);
        // dsm_->serve();
    }
    void thread_init(BLS &bls, size_t thread_nr) override
    {
        dsm_->registerThread();
        Base::thread_init(bls, thread_nr);
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        Base::on_start_bench(bls, conf);
    }
    void on_end_bench(const ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        Base::on_end_bench(res, bls, conf);
    }

    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        Base::on_thread_end_bench(bls, conf);
    }

    void benchmark_master(BLS &,
                          const Config &conf,
                          CoroContext &mctx,
                          ::bench::StopToken::pointer token,
                          bool) override
    {
        size_t coro_nr = conf.coro_nr();

        finished_coro_.current().clear();
        finished_coro_.current().resize(coro_nr, false);
        finished_nr_.current() = 0;

        auto &_dsm = this->__dsm();

        for (size_t i = 0; i < coro_nr; ++i)
        {
            mctx.yield_to_worker(i);
        }

        while (finished_nr_.current() < coro_nr)
        {
            char resp_buf[1024];
            _dsm.unreliable_recv(resp_buf, 1);
            AllocResponse *resp = (AllocResponse *) resp_buf;
            // LOG(WARNING) << "GOT RESP " << PRE(*resp);
            RpcContext *ctx = (RpcContext *) resp->hdr.rpc_context;
            // LOG(WARNING) << "Writing to " << (void *) ctx->data;
            *(uint64_t *) ctx->data = resp->addr;
            mctx.yield_to_worker(resp->hdr.from_coro_id);
        }

        CHECK(token->stop_requested());
    }

    void benchmark_coroutine(BLS &,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        auto dsm = get_dsm();
        [[maybe_unused]] auto tid = dsm->get_thread_id();
        [[maybe_unused]] auto cid = ctx.coro_id();
        size_t io_unit = conf.get<size_t>("alloc_size");
        size_t batch_nr = conf.get<size_t>("batch_factor");

        mem::Policy policy;
        // policy.flags = (flag_t) mem::AllocFlag::kMock;
        policy.batch_size_ = batch_nr * io_unit;
        while (!token->stop_requested())
        {
            auto ret = dsm->alloc2(io_unit, &ctx, policy);
            std::ignore = ret;
            token->complete_task(1);
        }
    }

private:
    DSM::pointer dsm_;
    std::vector<void *> huge_pages_;
    std::vector<ibv_mr *> irrelevant_mr_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    // f.configure_thread_nr({1, 4, 8, 16});
    f.configure_thread_nr({4});
    f.configure_coro_nr({8});
    f.add_option<size_t>("batch_factor", {1, 4, 8, 16});
    f.add_option<size_t>("alloc_size", {64});  // not important: use mock
    auto configs = f.generate_configs();

    exp.configure_monitor(20ms, 8);
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}