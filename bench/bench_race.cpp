#include <numa.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <thread>

#include "Common.h"
#include "DSMAdaptor.h"
#include "DSMCache.h"
#include "PerThread.h"
#include "WRLock.h"
#include "bench/DataFrame.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
#include "patronus/RdmaAdaptor.h"
#include "thirdparty/racehashing/hashtable.h"
#include "thirdparty/racehashing/hashtable_handle.h"
#include "thirdparty/racehashing/rhh_conf.h"
#include "util/Tracer.h"
#include "util/gflags_def.h"

using namespace patronus::hash;

using HashKey = ::hash::HashKey;

struct TLS
{
};

struct Spec
{
};

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using BLS = typename Base::BLS;
    using RaceHashingT = RaceHashing<4, 512, 8>;
    using HandleT = typename RaceHashingT::Handle;
    Experiment()
    {
        DSMConfig config;
        config.worker_nr = 0;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();

        // auto nid = dsm_->get_node_id();
        server_nid_ = dsm_->getClusterSize() - 1;
        if (is_server())
        {
            conf_.initial_subtable = 4;
            conf_.g_kvblock_pool_addr = (void *) dsm_->get_base_addr();
            conf_.g_kvblock_pool_size = dsm_->get_base_size();

            auto allocator = dsm_->get_dsm_allocator();
            auto server_rdma_ctx =
                DSMAdaptor::make_ptr(server_nid_, dsm_, nullptr /* ctx */);
            table_ =
                RaceHashingT::new_instance(server_rdma_ctx, allocator, conf_);
            auto meta_gaddr = table_->meta_gaddr();
            dsm_->put("race:meta_gaddr", meta_gaddr, 100ms);
            LOG(INFO) << "PUT: " << PRE(meta_gaddr);
            LOG(WARNING) << PRE(table_);
        }
        meta_gaddr_ = dsm_->get<GlobalAddress>("race:meta_gaddr", 100ms);
        LOG(INFO) << "GET: " << PRE(meta_gaddr_);
    }
    bool is_client() const
    {
        return dsm_->get_node_id() != server_nid_;
    }
    bool is_master_client() const
    {
        return dsm_->get_node_id() == 0;
    }
    bool is_server() const
    {
        return dsm_->get_node_id() == server_nid_;
    }

    typename HandleT::pointer get_handle(CoroContext *ctx, const Config &)
    {
        if (is_client())
        {
            auto handle_conf = RaceHashingConfigFactory::get_unprotected(
                "test", false /* force match */);
            auto adaptor = DSMAdaptor::make_ptr(server_nid_, dsm_, ctx);
            auto handle = HandleT::new_instance(
                meta_gaddr_, handle_conf, false /* auto expand */, adaptor);
            handle->init();
            return handle;
        }
        else
        {
            return nullptr;
        }
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
    void on_thread_start_bench(BLS &bls, const Config &conf) override
    {
        auto handle = get_handle(nullptr, conf);
        if (is_client())
        {
            auto key_rng = conf.get<size_t>("key_rng");
            auto kv_size = conf.get<size_t>("kv_size");
            auto preload_ratio = conf.get<size_t>("preload_ratio");
            auto preload_nr = key_rng * preload_ratio / 100.0;
            // client_nr not accurate: not counting client nodes
            auto client_nr = conf.thread_nr() * dsm_->getClusterSize();
            auto self_preload_nr = preload_nr / client_nr;
            std::string key(sizeof(uint64_t), ' ');
            std::string value(kv_size - sizeof(uint64_t), 'a');
            uint64_t &ukey = *(uint64_t *) key.data();
            for (size_t i = 0; i < self_preload_nr; ++i)
            {
                ukey = fast_pseudo_rand_int(key_rng);
                auto view = handle->prepare_kv(sizeof(uint64_t),
                                               kv_size - sizeof(uint64_t));
                view.fill_key_value(key, value);
                handle->put();
            }
        }
        Base::on_thread_start_bench(bls, conf);
    }
    void on_end_bench(const ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        df_.reg_result(res, conf);
        if (is_server())
        {
            LOG(WARNING) << PRE(table_);
        }

        Base::on_end_bench(res, bls, conf);
    }

    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        Base::on_thread_end_bench(bls, conf);
    }

    void exit() override
    {
        df_.dump(FLAGS_binary, FLAGS_exec_meta);
    }

    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        std::ignore = bls;

        auto key_rng = conf.get<size_t>("key_rng");
        auto get_ratio = conf.get<size_t>("get_ratio");
        auto delete_ratio = conf.get<size_t>("delete_ratio");
        auto kv_size = conf.get<size_t>("kv_size");

        std::string value(8, 'a');

        CHECK(dsm_->hasRegistered());

        std::string key(sizeof(uint64_t), ' ');
        uint64_t &ukey = *(uint64_t *) key.data();

        typename HandleT::pointer handle;
        if (is_client())
        {
            handle = get_handle(&ctx, conf);
        }
        while (!token->stop_requested())
        {
            if (is_client())
            {
                ukey = fast_pseudo_rand_int(key_rng - 1);

                bool is_get = fast_pseudo_bool_with_prob(get_ratio / 100.0);

                if (is_get)
                {
                    BufferView get_v;
                    auto view = handle->prepare_kv(sizeof(uint64_t), 0);
                    view.fill_key(key);
                    handle->get(get_v);
                }
                else
                {
                    bool is_delete =
                        fast_pseudo_bool_with_prob(delete_ratio / 100.0);
                    if (is_delete)
                    {
                        auto view = handle->prepare_kv(sizeof(uint64_t), 0);
                        view.fill_key(key);
                        handle->del();
                    }
                    else
                    {
                        auto view = handle->prepare_kv(
                            sizeof(uint64_t), kv_size - sizeof(uint64_t));
                        view.fill_key_value(key, value);
                        handle->put();
                    }
                }
                token->complete_task(1);
            }
        }
    }

private:
    DSM::pointer dsm_;
    size_t server_nid_;

    // server
    RaceHashingConfig conf_;
    typename RaceHashingT::pointer table_;  // for server
    GlobalAddress meta_gaddr_;

    bench::ResultDataFrame df_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    // f.configure_thread_nr({1, 2, 4, 8, 12, 16, 20, 24, 28, 32,
    // kMaxAppThread});

    f.configure_thread_nr({24});
    f.configure_coro_nr({3});

    f.add_option<size_t>("kv_size", {4_KB});
    f.add_option<size_t>("key_rng", {100_K});     // this is per client
    f.add_option<size_t>("preload_ratio", {50});  // this is per client
    f.add_option<size_t>("get_ratio", {80});
    f.add_option<size_t>("delete_ratio", {50});
    auto configs = f.generate_configs();

    exp.configure_monitor(1s, 10);
    // exp.disable_monitor();
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}