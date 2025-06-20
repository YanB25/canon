#include <city.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <queue>
#include <set>

#include "Common.h"
#include "DSMConfig.h"
#include "GlobalAddress.h"
#include "Timer.h"
#include "avis/avis.h"
#include "avis/config.h"
#include "avis/manager.h"
#include "avis/provider.h"
#include "bench/DataFrame.h"
#include "bench/config_factory.h"
#include "bench/experiment.h"
#include "bench/manager.h"
#include "bench/request.h"
#include "bench/token.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "sherman/RequestGenerator.h"
#include "sherman/Tree.h"
#include "util/DataFrameF.h"
#include "util/PerformanceReporter.h"
#include "util/Rand.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"

// clang-format off
// ./bench.py bench_tree --read_ratio=80 --put_ratio=20 --preload_ratio=20 --z=0 --warmup_ratio=20 --key_space=20000000 --dsm_gb=100
// clang-format on

DEFINE_double(z, 0.99, "The skewness.");
DEFINE_uint32(read_ratio, 0, "The read ratio [0, 100]");
DEFINE_uint32(put_ratio, 100, "The read ratio [0, 100]");
DEFINE_uint32(
    preload_ratio,
    5,
    "The preload ratio [0, 100]: how much data to load with single thread.");
DEFINE_uint32(warmup_ratio,
              100,
              "The warmup ratio [0, 100]: how much data to execute without "
              "reporting performance");
DEFINE_uint64(key_space, 100_M, "The key space. Set to several Ms");
// DEFINE_uint64(key_space, 1_M, "The key space. Set to several Ms");
DEFINE_uint32(pre_alloc_factor,
              1,
              "The number of pre-allocation. 1 means no pre-allocation, i.e., "
              "need RPC every time");
DEFINE_bool(canon, false, "Whether or not use Canon memory management");
DEFINE_uint32(dsm_gb, 40, "The size of DSM in GB");
DEFINE_uint32(cache_gb, 35, "The size of client's RDMA buffers in GB");
DEFINE_bool(subtree_locality, true, "Whether or not enable subtree locality");

using namespace util::literals;
using namespace std::chrono_literals;
using namespace sherman;

using IBenchConfig = ::bench::IBenchConfig;
using Tree = sherman::Tree;

using namespace hmdf;

struct Spec
{
};

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using Provider = avis::BuddyProvider;
    Experiment(const DSMConfig &config)
    {
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();

        server_nid_ = dsm_->getClusterSize() - 1;

        if (is_server())
        {
            auto tree_meta = dsm_->alloc_from(4_KB, server_nid_, 4_KB);
            dsm_->put("tree_meta", tree_meta, 100ms);
        }
        tree_meta_ = dsm_->get<GlobalAddress>("tree_meta", 100ms);

        if (FLAGS_canon)
        {
            if (is_server())
            {
                m_ = std::make_unique<avis::AvisManager>(
                    dsm_,
                    40 /* buddy_nr */,
                    2_MB /* pub size */,
                    std::nullopt /* no ARC */);
            }

            auto size = dsm_->get<uint32_t>("size", 100ms);
            providers_.resize(size);
            auto *raw = dsm_->get_raw("providers", 100ms);
            memcpy(
                providers_.data(), raw, providers_.size() * sizeof(Provider));

            pub_meta_ = dsm_->get<GlobalAddress>("pub", 100ms);
            pub_size_ = dsm_->get<size_t>("pub_size", 100ms);
        }

        TreeConfig tree_conf;
        tree_conf.allocate_batch_size_ =
            kInternalPageSize * FLAGS_pre_alloc_factor;
        if (FLAGS_canon)
        {
            tree_conf.avis_.emplace();
            tree_conf.avis_->providers = providers_;
            tree_conf.avis_->server_nid = server_nid_;
            tree_conf.avis_->pub_meta = pub_meta_;
            tree_conf.avis_->pub_size = pub_size_;
        }
        tree_ = Tree::new_instance(
            CHECK_NOTNULL(dsm_), tree_meta_, tree_conf, server_nid_);
    }

    void init() override
    {
        show_dsm_usage();
        ChronoTimer timer;
        if (dsm_->get_node_id() == 0)
        {
            size_t preload_nr = FLAGS_key_space * FLAGS_preload_ratio / 100.0;
            LOG(INFO) << "[master] Initial loading "
                      << util::pre_num(preload_nr) << "("
                      << util::pre_pcnt(FLAGS_preload_ratio / 100.0) << ")"
                      << " into the tree...";
            if (FLAGS_canon)
            {
                tree_->register_avis_handle(nullptr);
            }
            {
                for (size_t i = 1; i < preload_nr; ++i)
                {
                    auto key =
                        sherman::RequestGenBench::to_key(i, FLAGS_key_space);

                    // auto value_size = 8;
                    auto value_size = 128;
                    avis::Usage::ins().collect(value_size);
                    GlobalAddress value_raddr =
                        tree_->do_alloc(value_size, nullptr);
                    Buffer rdma_buf = dsm_->get_rdma_buffer(value_size);
                    memcpy(rdma_buf.buffer, &value_size, sizeof(value_size));
                    dsm_->prepare_write(rdma_buf.buffer,
                                        value_raddr,
                                        sizeof(uint64_t),
                                        false,
                                        nullptr);
                    // NOTE: no need to commit, just go on
                    uint64_t v = value_raddr.val;

                    tree_->prepare_alloc(nullptr);

                    tree_->insert(key, v);
                }
                LOG(INFO) << "[master] loading finished.";
            }
            if (FLAGS_canon)
            {
                tree_->reset_avis_handle(nullptr);
            }
        }
        auto ns = timer.pin();
        show_dsm_usage();
        LOG(INFO) << "Preload takes " << util::pre_ns(ns);

        Base::init();
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }

    void benchmark_master(BLS &,
                          const Config &conf,
                          CoroContext &mctx,
                          bench::StopToken::pointer token,
                          bool) override
    {
        size_t coro_nr = conf.coro_nr();
        size_t total_coro_nr = conf.effective_client_nr();

        finished_coro_.current().clear();
        finished_coro_.current().resize(coro_nr, false);
        finished_nr_.current() = 0;

        auto &_dsm = this->__dsm();

        for (size_t i = 0; i < coro_nr; ++i)
        {
            mctx.yield_to_worker(i);
        }

        while (total_finished_nr_.load(std::memory_order_relaxed) <
               total_coro_nr)
        {
            _dsm.try_master_coro_poll(&mctx, 1);

            // make these coros prioritized
            auto hot_waiting_coro = mctx.next_hot_waiting_coro();
            if (hot_waiting_coro)
            {
                mctx.yield_to_worker(*hot_waiting_coro);
            }
            if (!tree_->hot_wait_queue.empty())
            {
                auto next_coro_id = tree_->hot_wait_queue.front();
                tree_->hot_wait_queue.pop();
                mctx.yield_to_worker(next_coro_id);
            }
        }
        while (true)
        {
            auto hot_waiting_coro = mctx.next_hot_waiting_coro();
            if (hot_waiting_coro)
            {
                mctx.yield_to_worker(*hot_waiting_coro);
            }
            else
            {
                break;
            }
        }

        CHECK(token->stop_requested());
    }
    void benchmark_coroutine(BLS &bls,
                             const Config &config,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        auto &id = bls.id();
        bool is_master = id.is_master_thread() && id.is_master_worker_coro();
        bool use_avis = config.get<bool>("use_avis");

        ChronoTimer chrono_timer(is_master);

        auto min = std::chrono::nanoseconds(0ns).count();
        auto max = std::chrono::nanoseconds(20ms).count();
        auto step = std::chrono::nanoseconds(1us).count();
        OnePassBucketMonitor<uint64_t> lat_put(min, max, step);
        OnePassBucketMonitor<uint64_t> lat_get(min, max, step);
        OnePassBucketMonitor<uint64_t> lat_del(min, max, step);

        auto request_config =
            config.get<bench::RequestGenerator::Config>("request_config");
        bench::RequestGenerator g(request_config);

        if (use_avis)
        {
            tree_->register_avis_handle(&ctx);
        }
        uint64_t get_nr = 0;
        uint64_t ins_nr = 0;
        uint64_t del_nr = 0;

        while (!token->stop_requested())
        {
            if (!is_client())
            {
                continue;
            }
            // auto r = gen->next();
            auto req = g.next();
            uint64_t key = g.next_key();
            key = CityHash64((char *) &key, sizeof(key));

            if (is_master)
            {
                chrono_timer.pin();
            }

            if (req == bench::ReqType::kGet)
            {
                Value v;
                tree_->search(key, v, &ctx);
                get_nr++;
            }
            else if (req == bench::ReqType::kPut)
            {
                auto value_size = g.next_value_size();
                avis::Usage::ins().collect(value_size);
                GlobalAddress value_raddr = tree_->do_alloc(value_size, &ctx);
                Buffer rdma_buf = dsm_->get_rdma_buffer(value_size);
                memcpy(rdma_buf.buffer, &value_size, sizeof(value_size));
                dsm_->prepare_write(rdma_buf.buffer,
                                    value_raddr,
                                    sizeof(uint64_t),
                                    false,
                                    &ctx);
                // NOTE: no need to commit, just go on

                tree_->prepare_alloc(&ctx);

                tree_->insert(key, value_raddr.val, &ctx);

                dsm_->put_rdma_buffer(std::move(rdma_buf));
                ins_nr++;
            }
            else
            {
                DCHECK_EQ(req, bench::ReqType::kDel);
                tree_->del(key, &ctx);
                del_nr++;
            }

            if (is_master)
            {
                auto ns = chrono_timer.pin();
                token->collect_ns(ns);
                if (req == bench::ReqType::kGet)
                {
                    lat_get.collect(ns);
                }
                else if (req == bench::ReqType::kPut)
                {
                    lat_put.collect(ns);
                }
                else if (req == bench::ReqType::kDel)
                {
                    lat_del.collect(ns);
                }
                else
                {
                    LOG(FATAL) << "?";
                }
            }
            token->complete_task(1);
        }

        LOG_IF(INFO, !lat_put.empty()) << PRE(lat_put);
        LOG_IF(INFO, !lat_get.empty()) << PRE(lat_get);
        LOG_IF(INFO, !lat_del.empty()) << PRE(lat_del);

        if (is_master)
        {
            LOG(INFO) << PRE(get_nr, ins_nr, del_nr);

            auto *handle = tree_->get_avis_handle(&ctx);
            if (handle)
            {
                LOG(INFO) << "PTL induced RDMA: " << handle->ptl()->metric();
                LOG(INFO) << "BP induced RDMA: " << handle->ptl()->bp_metric();
                LOG(INFO) << "In cache: " << handle->dump();

                for (auto &buddy : handle->get_buddys())
                {
                    for (const auto &[order, lat] :
                         buddy.second->dump_latency())
                    {
                        LOG(INFO) << "Buddy(" << buddy.first << ") order "
                                  << order << ": " << lat;
                    }
                }
            }
        }
        if (is_master)
        {
            ChronoTimer timer;
            auto [level_nr, node_nr] = tree_->print_and_check_tree(&ctx);
            auto ns = timer.pin();
            LOG(INFO) << "Check tree takes: " << util::pre_ns(ns) << ". Level "
                      << level_nr << ", node: " << node_nr;
        }

        if (use_avis)
        {
            tree_->reset_avis_handle(&ctx);
        }
    }

    void on_thread_start_bench(BLS &bls, const Config &config) override
    {
        bool use_avis = config.get<bool>("use_avis");
        if (is_client())
        {
            if (!has_warm_up_)
            {
                auto &id = bls.id();
                size_t warmup_thread = config.thread_nr();

                uint64_t end_warm_key =
                    FLAGS_key_space * FLAGS_warmup_ratio / 100.0;
                uint64_t all_thread =
                    warmup_thread * (dsm_->getClusterSize() - 1);
                uint64_t my_id =
                    warmup_thread * dsm_->get_node_id() + id.thread_id;
                LOG_IF(INFO, id.is_master_thread())
                    << "Warn up with " << all_thread
                    << " threads. Amount: " << util::pre_num(end_warm_key);
                // LOG(INFO) << "I am " << my_id << ". Warming up...";
                if (use_avis)
                {
                    tree_->register_avis_handle(nullptr);
                }
                size_t finished_nr = 0;
                size_t expect_nr = end_warm_key / all_thread;
                auto now = std::chrono::steady_clock::now();
                for (uint64_t i = 1; i < end_warm_key; ++i)
                {
                    auto then = std::chrono::steady_clock::now();
                    if (then - now >= 1s)
                    {
                        now = then;
                        LOG(INFO)
                            << "[bench] warming up " << finished_nr << " / "
                            << expect_nr << " ("
                            << util::pre_pcnt(1.0 * finished_nr / expect_nr)
                            << ")";
                    }

                    if (i % all_thread == my_id)
                    {
                        auto value_size = 128;
                        avis::Usage::ins().collect(value_size);
                        GlobalAddress value_raddr =
                            tree_->do_alloc(value_size, nullptr);
                        Buffer rdma_buf = dsm_->get_rdma_buffer(value_size);
                        memcpy(
                            rdma_buf.buffer, &value_size, sizeof(value_size));
                        dsm_->prepare_write(rdma_buf.buffer,
                                            value_raddr,
                                            sizeof(uint64_t),
                                            false,
                                            nullptr);

                        // NOTE: no need to commit, just go on
                        uint64_t v = value_raddr.val;

                        tree_->prepare_alloc(nullptr);
                        tree_->insert(sherman::RequestGenBench::to_key(
                                          i, FLAGS_key_space),
                                      v);
                        finished_nr++;
                    }
                }
                if (use_avis)
                {
                    tree_->reset_avis_handle(nullptr);
                }
            }
        }
        has_warm_up_ = true;

        Base::on_thread_start_bench(bls, config);
    }

    void show_dsm_usage()
    {
        LOG(INFO) << dsm_->dsm_usage();
    }
    void on_start_bench(BLS &bls, const Config &config) override
    {
        LOG(INFO) << "[bench] start bench: " << config;
        auto bp_ptl = config.get<bool>("use_bp_ptl");
        auto postorder = config.get<bool>("postorder");
        avis::Config::ins().configure_bp(bp_ptl);
        avis::Config::ins().configure_ptl(bp_ptl);
        avis::Config::ins().configure_buddy_use_post_order(postorder);

        show_dsm_usage();
        Base::on_start_bench(bls, config);
    }

    void on_end_bench(const ::bench::ResultRecord &results,
                      BLS &bls,
                      const Config &conf) override
    {
        df.reg_result(results, conf);

        if (is_client())
        {
            if (!providers_.empty())
            {
                auto dumper = std::make_shared<avis::Dumper>(dsm_, providers_);
                dumper->report(false);
                auto [used, total] = dumper->bitmap_utilizations();
                LOG(INFO) << "Bitmap usage: " << util::pre_byte(used) << " / "
                          << util::pre_byte(total) << " ("
                          << used * 100.0 / total << "% )";
            }

            uint64_t global_usage = avis::Usage::ins().sum(dsm_);
            LOG(INFO) << "Actual memory usage: "
                      << util::pre_byte(global_usage);
        }

        show_dsm_usage();
        Base::on_end_bench(results, bls, conf);
    }

    void exit() override
    {
        // df.dump(FLAGS_binary.c_str(), FLAGS_exec_meta);
        Base::exit();
    }
    bool is_server() const
    {
        return dsm_->get_node_id() == server_nid_;
    }
    bool is_client() const
    {
        return !is_server();
    }

private:
    DSM::pointer dsm_;
    Tree::pointer tree_;
    size_t server_nid_{};

    GlobalAddress tree_meta_;

    bench::ResultDataFrame df;

    std::unique_ptr<avis::AvisManager> m_;
    std::vector<avis::BuddyProvider> providers_;
    GlobalAddress pub_meta_;
    size_t pub_size_;

    bool has_warm_up_{false};
};

// With 6 nodes, write-only, uniform, max_key = 16_M
// The cluster memory uses ~210 MB, local 44 MB
int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    bench::ConfigFactory f;
    // f.configure_thread_nr({1, 2, 4, 8, 16, 32});
    // f.configure_thread_nr({1, 2});
    // f.configure_thread_nr({24, 32});
    // f.configure_thread_nr({8});
    // f.configure_thread_nr({4, 8});
    f.configure_thread_nr({16, 32});
    f.configure_coro_nr({3});

    double put_ratio = FLAGS_put_ratio / 100.0;
    double get_ratio = FLAGS_read_ratio / 100.0;
    double del_ratio = 1.0 - put_ratio - get_ratio;
    bench::RequestGenerator::Config request_config{
        .config{
            .put_get_del =
                {
                    put_ratio,
                    get_ratio,
                    del_ratio,
                },
            .key_size_dist = {{8, 1.0}},
            .z = FLAGS_z,
            .key_rng = FLAGS_key_space,
        },
        .value_size_dist =
            std::vector<std::pair<size_t, double>>{
                {8, 0.2}, {32, 0.2}, {64, 0.2}, {128, 0.2}, {256, 0.2}},
        .value_size_model = {},
    };

    f.add_option<bench::RequestGenerator::Config>("request_config",
                                                  {request_config});

    f.add_option<bool>("use_avis", {FLAGS_canon});
    f.add_option<size_t>("preload_ratio", {FLAGS_preload_ratio});
    f.add_option<size_t>("warmup_ratio", {FLAGS_warmup_ratio});
    f.add_option<bool>("use_bp_ptl", {false});
    f.add_option<bool>("postorder", {FLAGS_subtree_locality});

    DSMConfig config;
    // config.worker_nr = 0;
    if (std::stoi(FLAGS_node_id) == FLAGS_machine_nr - 1)
    {
        config.dsmSize = FLAGS_dsm_gb * 1_GB;
        config.cacheConfig.cacheSize = 1_GB;
    }
    else
    {
        config.dsmSize = 1_GB;
        config.cacheConfig.cacheSize = FLAGS_cache_gb * 1_GB;
    }
    if (FLAGS_canon)
    {
        avis::Config::ins().configure_bitmap_size_custom();
    }

    Experiment exp(config);
    exp.configure_monitor(100ms, 60);
    exp.launch_coroutines(f.generate_configs());

    LOG(INFO) << "Done.";
}
