#include <numa.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <thread>

#include "Common.h"
#include "DSMAdaptor.h"
#include "DSMCache.h"
#include "GlobalAddress.h"
#include "PerThread.h"
#include "WRLock.h"
#include "bench/DataFrame.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
#include "patronus/RdmaAdaptor.h"
#include "thirdparty/racehashing/debug.h"
#include "thirdparty/racehashing/hashtable.h"
#include "thirdparty/racehashing/hashtable_handle.h"
#include "thirdparty/racehashing/kv_block.h"
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
            auto meta_raddr = meta_gaddr;
            dsm_->put("race:meta_raddr", meta_raddr, 100ms);
            LOG(INFO) << "PUT: " << PRE(meta_raddr);
            LOG(WARNING) << PRE(table_);
        }
        meta_raddr_ = dsm_->get<GlobalAddress>("race:meta_raddr", 100ms);
        LOG(INFO) << "GET: " << PRE(meta_raddr_);
    }
    bool is_client() const
    {
        // return dsm_->get_node_id() != server_nid_;
        return dsm_->get_node_id() == 0;
    }
    bool is_server() const
    {
        return dsm_->get_node_id() == server_nid_;
    }

    typename HandleT::pointer get_handle(CoroContext *ctx)
    {
        if (is_client())
        {
            auto handle_conf = RaceHashingConfigFactory::get_unprotected(
                "test", false /* force match */);
            auto adaptor = DSMAdaptor::make_ptr(server_nid_, dsm_, ctx);
            auto handle = HandleT::new_instance(
                meta_raddr_, handle_conf, false /* auto expand */, adaptor);
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
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        Base::on_start_bench(bls, conf);
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
        if (unlikely(err_))
        {
            LOG(INFO) << "!!!! find the error!!";
            LOG(ERROR) << PRE(hkey_);
            using namespace ranges;

            auto f_with_key = views::filter(
                [this](const auto &r)
                { return r->key == hkey_ || r->raddr == raddr_; });

            auto all_his_with_key = ::hash::history.take(20, f_with_key);

            LOG(INFO) << PRE(all_his_with_key) << "$.";

            auto target =
                all_his_with_key |
                views::filter([](const auto &r) { return r->ma == kQuery; });

            GlobalAddress kvblock_gaddr;
            auto gaddr = raddr_;
            kvblock_gaddr.nodeID = gaddr.nodeID;
            {
                CHECK(!target.empty());
                auto data = target.front();
                LOG(INFO) << PRE(data);
                auto gaddr = data->raddr;
                auto buffer = dsm_->get_rdma_buffer(64);
                dsm_->prepare_read(buffer.buffer, gaddr, 8, false, nullptr);
                dsm_->commit(nullptr);
                LOG(INFO) << std::endl << util::Hexdump(buffer.buffer, 64);
                uint64_t value = *(uint64_t *) buffer.buffer;
                util::TaggedPtrImpl<void> ptr(value);
                LOG(INFO) << PRE((void *) (uint64_t) ptr.u8_h()) << ", "
                          << PRE((void *) (uint64_t) ptr.u8_l()) << ", "
                          << PRE((void *) ptr.ptr());

                auto hash = hash_impl(err_key_.data(), err_key_.size());
                auto m = hash_m(hash);
                auto fp = hash_fp(hash);
                LOG(INFO) << PRE((void *) m) << ", " << PRE((void *) fp);
                CHECK_EQ(fp, ptr.u8_h()) << "** aha! fp mismatch";

                kvblock_gaddr.offset = (uint64_t) ptr.ptr();
                dsm_->put_rdma_buffer(std::move(buffer));
            }
            {
                auto kvblock_buffer = dsm_->get_rdma_buffer(64);
                LOG(ERROR) << "Fetching buffer: " << PRE(kvblock_gaddr);
                dsm_->prepare_read(
                    kvblock_buffer.buffer, kvblock_gaddr, 64, false, nullptr);
                dsm_->commit(nullptr);
                KVBlock &block = *(KVBlock *) kvblock_buffer.buffer;
                LOG(INFO) << PRE(block.key_len) << ", " << PRE(block.value_len)
                          << ", " << PRE((void *) block.hash);
                std::string got_key(block.key_len, ' ');
                std::string got_value(block.value_len, ' ');
                memcpy(got_key.data(), block.buf, block.key_len);
                memcpy(got_value.data(),
                       block.buf + block.key_len,
                       block.value_len);
                LOG(INFO) << PRE(got_key) << ", " << PRE(got_value);
                if (got_key == err_key_)
                {
                    LOG(INFO) << "** match!!";
                }
                else
                {
                    LOG(FATAL) << "** mismatch!!";
                }
                dsm_->put_rdma_buffer(std::move(kvblock_buffer));
            }
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
        // auto tid = dsm_->get_thread_id();
        std::ignore = bls;

        auto kv_size = conf.get<size_t>("kv_size");
        auto key_rng = conf.get<size_t>("key_rng");
        auto get_ratio = conf.get<size_t>("get_ratio");
        auto delete_ratio = conf.get<size_t>("delete_ratio");

        CHECK(dsm_->hasRegistered());

        std::unordered_map<std::string, std::string> kvs;

        std::string key(sizeof(HashKey), ' ');
        HashKey &hkey = *(HashKey *) key.data();
        hkey.nid = dsm_->get_node_id();
        hkey.tid = dsm_->get_thread_id();
        hkey.cid = ctx.coro_id();
        std::string value(8, 'a');

        auto expect_value_size =
            KVBlockView::kvblock_fit_value(kv_size, key.size());

        util::TraceManager tm;

        while (!token->stop_requested())
        {
            if (is_client())
            {
                auto handle = get_handle(&ctx);

                hkey.value = fast_pseudo_rand_int(key_rng - 1);
                fast_pseudo_fill_buf(value.data(), value.size());

                bool is_get = fast_pseudo_bool_with_prob(get_ratio / 100.0);
                bool is_delete =
                    fast_pseudo_bool_with_prob(delete_ratio / 100.0);

                if (is_get)
                {
                    BufferView get_v;
                    // auto trace = tm.trace("get");
                    // trace.set("key", key);
                    // auto rc = handle->get(key, get_v, trace);
                    if constexpr (kEnableHistory)
                    {
                        ::hash::history.current().add(Record{
                            .ma = kEvent,
                            .ua = kRead,
                            .raddr = GlobalAddress{},
                            .key = hkey,
                        });
                    }
                    auto view = handle->prepare_kv(key.size(), 0);
                    view.fill_key(key);
                    auto rc = handle->get(get_v);
                    if constexpr (kEnableHistory)
                    {
                        ::hash::history.current().add(Record{
                            .ma = kEvent,
                            .ua = kRead,
                            .raddr = GlobalAddress(0, (uintptr_t) 0xff),
                            .key = hkey,
                        });
                    }
                    if (kvs.count(key))
                    {
                        if (unlikely(rc != RC::kOk ||
                                     !get_v.to_sv().starts_with(kvs[key])))
                        {
                            token->core().request_stop();

                            // wait for everyone has finished
                            std::this_thread::sleep_for(100ms);

                            std::lock_guard<std::mutex> mu(lk_);
                            if (!err_)
                            {
                                LOG(ERROR)
                                    << "expect " << PRE(RC::kOk) << ", got "
                                    << PRE(rc) << ", expect " << PRE(kvs[key])
                                    << ", got " << PRE(get_v);
                                err_ = true;
                                err_key_ = key;
                                hkey_ = hkey;

                                using namespace ranges;

                                auto filter = views::filter(
                                    [this](const auto &r) {
                                        return r.t().key == hkey_ &&
                                               r.t().ma == kQuery;
                                    });

                                auto f =
                                    ::hash::history.current().take(20, filter);

                                if (unlikely(f.empty()))
                                {
                                    LOG(FATAL)
                                        << "Error on unwrap: history empty: ";
                                }

                                raddr_ = f.front()->raddr;
                                LOG(ERROR) << PRE(raddr_);
                            }
                        }
                    }
                    else
                    {
                        CHECK_EQ(rc, RC::kNotFound);
                    }
                }
                else if (is_delete)
                {
                    auto view = handle->prepare_kv(key.size(), 0);
                    view.fill_key(key);
                    auto rc = handle->del();
                    if (kvs.count(key))
                    {
                        CHECK_EQ(rc, RC::kOk);
                        kvs.erase(key);
                    }
                    else
                    {
                        CHECK_EQ(rc, RC::kNotFound);
                    }
                }
                else
                {
                    // auto trace = tm.trace("put");
                    // trace.set("key", key);
                    // handle->put(key, value, trace).expect(RC::kOk);
                    if constexpr (kEnableHistory)
                    {
                        ::hash::history.current().add(Record{
                            .ma = kEvent,
                            .ua = kCAS,
                            .raddr = GlobalAddress{},
                            .key = hkey,
                        });
                    }
                    auto view =
                        handle->prepare_kv(key.size(), expect_value_size);
                    view.fill_key_value(key, value);
                    auto rc = handle->put();
                    if (likely(rc == RC::kOk))
                    {
                        kvs[key] = value;
                    }
                    else
                    {
                        CHECK_EQ(rc, RC::kNoMem);
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
    GlobalAddress meta_raddr_;

    std::mutex lk_;
    Perthread<std::unordered_map<std::string, std::list<std::string>>> history_;
    std::string err_key_;
    HashKey hkey_;
    GlobalAddress raddr_;
    bool err_{false};

    bench::ResultDataFrame df_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    // f.configure_thread_nr({1, 2, 4, 8, 12, 16, 20, 24, 28, 32,
    // kMaxAppThread});
    f.configure_thread_nr({kMaxAppThread});
    f.add_option<size_t>("kv_size", {64});
    f.add_option<size_t>("key_rng", {512});  // this is per client
    f.add_option<size_t>("get_ratio", {80});
    f.add_option<size_t>("delete_ratio", {50});
    f.configure_coro_nr({3});
    auto configs = f.generate_configs();

    exp.configure_monitor(1s, 10);
    // exp.disable_monitor();
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}