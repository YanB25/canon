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
    struct GLS
    {
        std::atomic<int> gls{};
    };
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    using StorageT = ::bench::Storage<Spec>;
    void on_thread_start_bench(StorageT &storage, const Config &conf) override
    {
        auto &gls = storage.global();
        auto &tls = storage.thread();
        gls.gls.fetch_add(1);
        tls.tls.fetch_add(1);
        LOG(INFO) << "on thread start: " << PRE(gls.gls) << ", " << PRE(tls.tls)
                  << " from " << util::get_thread_id() << ": "
                  << PRE(storage.id());
        CHECK_NE(storage.id().thread_id, -1);
        Base::on_thread_start_bench(storage, conf);
    }

    void thread_init(BLS &bls, size_t) override
    {
        LOG(INFO) << "thread_init " << PRE(bls.id());
    }

    void on_coro_start_bench(StorageT &store, const Config &conf) override
    {
        auto &gls = store.global();
        auto &tls = store.thread();
        gls.gls.fetch_add(1);
        tls.tls.fetch_add(1);
        LOG(INFO) << "on coro start: " << PRE(gls.gls) << ", " << PRE(tls.tls)
                  << ", "
                  << " from " << util::get_thread_id() << ": "
                  << PRE(store.id());
        CHECK_NE(store.id().thread_id, -1);
        CHECK_NE(store.id().coro_id, -1);
        Base::on_coro_start_bench(store, conf);
    }
    void on_coro_end_bench(StorageT &store, const Config &conf) override
    {
        auto &gls = store.global();
        auto &tls = store.thread();

        auto thread_nr = conf.thread_nr();
        CHECK_EQ(gls.gls, thread_nr);
        CHECK_EQ(tls.tls, 1);

        LOG(INFO) << "on_coro_end_bench " << PRE(store.id());
        CHECK_NE(store.id().thread_id, -1);
        CHECK_NE(store.id().coro_id, -1);

        Base::on_coro_end_bench(store, conf);
    }
    void on_thread_end_bench(StorageT &store, const Config &conf) override
    {
        auto &gls = store.global();
        auto &tls = store.thread();

        auto thread_nr = conf.thread_nr();
        CHECK_EQ(gls.gls, thread_nr);
        CHECK_EQ(tls.tls, 1);
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
        CHECK_EQ(gls.gls, thread_nr);
        LOG(INFO) << "on_end_bench " << PRE(bls.id());
        CHECK(bls.id().is_master_thread());
        Base::on_end_bench(res, bls, conf);
    }
    virtual void benchmark(StorageT &bls,
                           const Config &conf,
                           ::bench::StopToken::pointer token,
                           bool) override
    {
        auto &gls = bls.global();
        auto &tls = bls.thread();
        auto thread_nr = conf.thread_nr();
        CHECK_EQ(gls.gls, thread_nr)
            << "`thread_nr` for on_thread_start_bench, for each thread, "
               "`coro_nr + 1` for on_coro_start_bench";
        CHECK_EQ(tls.tls, 1)
            << "`one` for on_thread_start_bench, `coro_nr + 1` for "
               "on_coro_start_bench";

        LOG(INFO) << "benchmark " << PRE(bls.id());

        CHECK_NE(bls.id().thread_id, -1);

        token->core().request_stop();
    }

private:
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
    // f.configure_thread_nr({1, 3, 8, 16, kMaxAppThread});
    f.configure_thread_nr({1, 3});
    Experiment exp;

    exp.launch(f.generate_configs());

    LOG(INFO) << "PASS.";
}