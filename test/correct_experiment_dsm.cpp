#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "DSM.h"
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

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using StorageT = ::bench::Storage<Spec>;
    Experiment()
    {
        DSMConfig config;
        config.worker_nr = 0;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
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
        CHECK_NE(store.id().node_id, -1);
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
        CHECK_NE(store.id().node_id, -1);
        CHECK_NE(store.id().thread_id, -1);
        CHECK_NE(store.id().coro_id, -1);
        Base::coro_init(store, coro_nr);
    }
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        CHECK_NE(bls.id().node_id, -1);
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
        CHECK_NE(storage.id().node_id, -1);
        CHECK_NE(storage.id().thread_id, -1);

        {
            has_coro_master_.current() = true;
            worker_coro_id_.current().clear();
        }

        Base::on_thread_start_bench(storage, conf);
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
        CHECK_NE(store.id().node_id, -1);
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

        CHECK_NE(store.id().node_id, -1);
        CHECK_NE(store.id().thread_id, -1);
        CHECK_NE(store.id().coro_id, -1);
        LOG(WARNING) << "on_coro_end_bench " << PRE(store.id());

        auto id = store.id();
        {
            std::lock_guard<std::mutex> lk(mu_.current());
            CHECK_NE(id.coro_id, kNotACoro);
            if (id.is_master_coro())
            {
                CHECK(!id.is_worker_coro());
                CHECK_EQ(id.coro_id, kMasterCoro);
                has_coro_master_.current() = true;
            }
            else
            {
                CHECK(id.is_worker_coro());
                auto cid = id.coro_id;
                CHECK_EQ(worker_coro_id_.current().count(cid), 0);
                CHECK(worker_coro_id_.current().emplace(cid).second);
            }
        }

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

        CHECK_NE(store.id().node_id, -1);
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

        CHECK_NE(bls.id().node_id, -1);
        CHECK(bls.id().is_master_thread());

        {
            LOG(INFO) << "validating on_end_bench...";
            CHECK(has_coro_master_.current());
            CHECK_EQ(worker_coro_id_.current().size(), conf.coro_nr());
        }

        Base::on_end_bench(res, bls, conf);
    }
    void benchmark_coroutine(StorageT &bls,
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

        CHECK_NE(bls.id().node_id, -1);
        CHECK_NE(bls.id().thread_id, -1);
        CHECK_NE(bls.id().coro_id, -1);
        CHECK(bls.id().is_worker_coro());
        CHECK(!bls.id().is_master_coro());

        ctx.record_yield_reason(true /* exit */);
        ctx.yield_to_master();
    }

    void benchmark_master(StorageT &bls,
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

        CHECK_NE(bls.id().node_id, -1);
        CHECK_NE(bls.id().thread_id, -1);
        CHECK_NE(bls.id().coro_id, -1);

        CHECK(bls.id().is_master_coro());
        CHECK(!bls.id().is_worker_coro());

        token->core().request_stop();
    }

private:
    std::atomic<int> alloc_{0};
    DSM::pointer dsm_;

    Perthread<std::mutex> mu_;
    Perthread<bool> has_coro_master_{};
    Perthread<std::set<int>> worker_coro_id_{};
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