#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "bench/experiment.h"
#include "bench/local_experiment.h"
#include "bench/storage.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/History.h"
#include "util/RingBuffer.h"
#include "util/gflags_def.h"

DEFINE_string(msg, "hello workd", "the message");

struct Spec
{
    struct TLS
    {
        std::atomic<int> tls{};
    };
    struct PTLS
    {
        std::atomic<int> ptls{};
    };
    struct CLS
    {
        std::atomic<int> cls{};
    };
    struct GLS
    {
        std::atomic<int> gls{};

        // for checking PCLS
        std::mutex mu;
        std::set<int> values;
    };
    struct PCLS
    {
        std::atomic<int> pcls{};
    };
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    using StorageT = ::bench::Storage<Spec>;
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        CHECK(bls.id().is_master_thread());
        Base::on_start_bench(bls, conf);
    }
    void on_thread_start_bench(StorageT &storage, const Config &conf) override
    {
        auto &gls = storage.global();
        auto &tls = storage.thread();
        gls.gls.fetch_add(1);
        tls.tls.fetch_add(1);
        LOG(INFO) << "on thread start: " << PRE(gls.gls) << ", " << PRE(tls.tls)
                  << " from " << util::get_thread_id();
        CHECK_NE(storage.id().thread_id, -1);
        Base::on_thread_start_bench(storage, conf);
    }
    void init() override
    {
        Base::init();
    }
    void thread_init(StorageT &store, size_t thread_nr) override
    {
        auto &ptls = store.persistent_thread();
        ptls.ptls = util::get_thread_id();
        LOG(INFO) << (void *) &ptls.ptls << " set to " << util::get_thread_id();
        LOG(INFO) << "thread_init " << PRE(store.id());
        CHECK_NE(store.id().thread_id, -1);
        Base::thread_init(store, thread_nr);
    }
    void coro_init(StorageT &store, size_t coro_nr) override
    {
        auto &pcls = store.persistent_coroutine();
        auto &ptls = store.persistent_thread();
        pcls.pcls = alloc_.fetch_add(1);
        LOG(INFO) << PRE(pcls.pcls) << " at " << (void *) &pcls.pcls;
        CHECK_EQ(ptls.ptls, util::get_thread_id());
        LOG(INFO) << "coro_init " << PRE(store.id());
        CHECK_NE(store.id().thread_id, -1);
        CHECK_NE(store.id().coro_id, -1);
        Base::coro_init(store, coro_nr);
    }
    void on_coro_start_bench(StorageT &store, const Config &conf) override
    {
        auto &gls = store.global();
        auto &tls = store.thread();
        auto &cls = store.coroutine();
        gls.gls.fetch_add(1);
        cls.cls.fetch_add(1);
        tls.tls.fetch_add(1);
        LOG(INFO) << "on coro start: " << PRE(gls.gls) << ", " << PRE(tls.tls)
                  << ", " << PRE(cls.cls) << " from " << util::get_thread_id();
        LOG(INFO) << "on_coro_start_bench " << PRE(store.id());
        CHECK_NE(store.id().thread_id, -1);
        CHECK_NE(store.id().coro_id, -1);
        Base::on_coro_start_bench(store, conf);
    }
    void on_coro_end_bench(StorageT &store, const Config &conf) override
    {
        auto &gls = store.global();
        auto &tls = store.thread();
        auto &cls = store.coroutine();

        auto thread_nr = conf.thread_nr();
        auto coro_nr = conf.coro_nr();
        CHECK_EQ(gls.gls, thread_nr + thread_nr * (coro_nr + 1));
        CHECK_EQ(tls.tls, 1 + coro_nr + 1);
        CHECK_EQ(cls.cls, 1);

        CHECK_NE(store.id().thread_id, -1);
        CHECK_NE(store.id().coro_id, -1);
        LOG(INFO) << "on_coro_end_bench " << PRE(store.id());

        Base::on_coro_end_bench(store, conf);
    }
    void on_thread_end_bench(StorageT &store, const Config &conf) override
    {
        auto &gls = store.global();
        auto &tls = store.thread();

        auto thread_nr = conf.thread_nr();
        auto coro_nr = conf.coro_nr();
        CHECK_EQ(gls.gls, thread_nr + thread_nr * (coro_nr + 1));
        CHECK_EQ(tls.tls, 1 + coro_nr + 1);

        LOG(INFO) << "on_thread_end_bench " << PRE(store.id());

        CHECK_NE(store.id().thread_id, -1);

        Base::on_thread_end_bench(store, conf);
    }
    void on_end_bench(const ::bench::ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        auto &gls = bls.global();

        auto thread_nr = conf.thread_nr();
        auto coro_nr = conf.coro_nr();
        CHECK_EQ(gls.gls, thread_nr + thread_nr * (coro_nr + 1));

        LOG(INFO) << "on_end_bench " << PRE(bls.id());

        CHECK(bls.id().is_master_thread());

        Base::on_end_bench(res, bls, conf);
    }
    virtual void benchmark_worker(StorageT &bls,
                                  const Config &conf,
                                  CoroContext &ctx,
                                  ::bench::StopToken::pointer,
                                  bool) override
    {
        auto &gls = bls.global();
        auto &tls = bls.thread();
        auto &cls = bls.coroutine();
        auto &pcls = bls.persistent_coroutine();
        auto &ptls = bls.persistent_thread();
        auto thread_nr = conf.thread_nr();
        auto coro_nr = conf.coro_nr();
        CHECK_EQ(gls.gls, thread_nr + thread_nr * (coro_nr + 1))
            << "`thread_nr` for on_thread_start_bench, for each thread, "
               "`coro_nr + 1` for on_coro_start_bench";
        CHECK_EQ(tls.tls, 1 + coro_nr + 1)
            << "`one` for on_thread_start_bench, `coro_nr + 1` for "
               "on_coro_start_bench";
        CHECK_EQ(cls.cls, 1) << "`one` for on_coro_start_bench";
        CHECK_EQ(ptls.ptls, util::get_thread_id());

        {
            std::lock_guard<std::mutex> lock(gls.mu);
            CHECK_EQ(gls.values.count(pcls.pcls), 0);
            gls.values.insert(pcls.pcls);
            CHECK_GE(pcls.pcls, 0);
            CHECK_LT(pcls.pcls, (define::kMaxCoroNr + 1) * kMaxAppThread)
                << (void *) &pcls.pcls;
        }
        LOG(INFO) << "bench " << PRE(bls.id());

        CHECK_NE(bls.id().thread_id, -1);
        CHECK_NE(bls.id().coro_id, -1);

        ctx.record_yield_reason(true /* exit */);
        ctx.yield_to_master();
    }

    virtual void benchmark_master(StorageT &bls,
                                  const Config &conf,
                                  CoroContext &ctx,
                                  ::bench::StopToken::pointer token,
                                  bool) override
    {
        auto &gls = bls.global();
        auto &tls = bls.thread();
        auto &cls = bls.coroutine();
        auto &ptls = bls.persistent_thread();
        auto thread_nr = conf.thread_nr();
        auto coro_nr = conf.coro_nr();
        CHECK_EQ(gls.gls, thread_nr + thread_nr * (coro_nr + 1))
            << "`thread_nr` for on_thread_start_bench, for each thread, "
               "`coro_nr + 1` for on_coro_start_bench";
        CHECK_EQ(tls.tls, 1 + coro_nr + 1)
            << "`one` for on_thread_start_bench, `coro_nr + 1` for "
               "on_coro_start_bench";
        CHECK_EQ(cls.cls, 1) << "`one` for on_coro_start_bench";
        CHECK_EQ(ptls.ptls, util::get_thread_id())
            << "at " << (void *) &ptls.ptls;

        for (size_t i = 0; i < conf.coro_nr(); ++i)
        {
            ctx.yield_to_worker(i);
        }

        token->core().request_stop();
    }

private:
    std::atomic<int> alloc_{0};
};

struct StorageSpec
{
    using GLS = int;
    using TLS = double;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    ::bench::ConfigFactory f;
    // f.configure_thread_nr({1, 3, kMaxAppThread});
    // f.configure_coro_nr({1, 3, 8, define::kMaxCoroNr});
    f.configure_thread_nr({1, 3});
    f.configure_coro_nr({1, 3});
    Experiment exp;

    exp.launch_coroutines(f.generate_configs());

    LOG(INFO) << "PASS.";
}