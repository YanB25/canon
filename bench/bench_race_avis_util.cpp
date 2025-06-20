#include <chrono>
#include <limits>
#include <thread>

#include "Common.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "avis/AvisAdaptor.h"
#include "avis/avis.h"
#include "avis/bench/hashtable.h"
#include "avis/config.h"
#include "avis/dump.h"
#include "avis/handle.h"
#include "avis/publisher.h"
#include "bench/experiment.h"
#include "bench/request.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "sherman/Tree.h"
#include "thirdparty/racehashing/hashtable.h"
#include "thirdparty/racehashing/race.h"
#include "thirdparty/racehashing/utils.h"
#include "util/PerformanceReporter.h"
#include "util/PreUtil.h"
#include "util/Rand.h"
#include "util/gflags_dec.h"
#include "util/gflags_def.h"

DEFINE_uint64(block_mb, 16, "The size of block in MB");
DEFINE_uint32(thread_nr, 32, "The number of thread per machine");
DEFINE_uint32(coro_nr, 1, "The number of coro per thread");
DEFINE_bool(share, false, "Whether or not allow sharing bitmap");
DEFINE_bool(partition, false, "Whether or not use memory partitioning");
DEFINE_bool(canon, false, "Whether or not use Canon");
DEFINE_string(trace_file, "null", "Please ignore me");

DEFINE_uint32(buddy_nr, 60, "The number of buddy trees.");
DEFINE_uint32(cache_gb, 30, "The size of RDMA cache in GB.");
DEFINE_uint32(dsm_gb, 160, "The size of DSM buffer in GB.");

using namespace patronus::hash;

constexpr static size_t kE = 64;
constexpr static size_t kB = 1024 * 8;
constexpr static size_t kS = 8;

using Provider = avis::BuddyProvider;

// using RaceHashingT = RaceHashing<kE, kB, kS>;
// using HandleT = typename RaceHashingT::Handle;
using TableT = RaceHashing<kE, kB, kS>;

using HashTableT = HashTable<kE, kB, kS>;
using HashClientT = HashClient<kE, kB, kS>;

constexpr static auto kTableCapacity = TableT::max_capacity();

struct Spec
{
    struct CLS
    {
        std::shared_ptr<HashClientT> handle_;
    };
    struct PCLS
    {
        std::shared_ptr<avis::AvisAdaptor> avis_adaptor_;
        size_t op_nr = 0;
    };
};

struct Dist
{
    double put_rate;
    double del_rate;
};
inline std::ostream &operator<<(std::ostream &os, const Dist &d)
{
    os << fmt::format("{}-{}", d.put_rate, d.del_rate);
    return os;
}

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using BLS = typename Base::BLS;

    using Provider = avis::BuddyProvider;
    Experiment(const DSMConfig &config)
    {
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();

        server_nid_ = dsm_->getClusterSize() - 1;

        if (is_server())
        {
            auto buddy_nr = FLAGS_buddy_nr;
            if (FLAGS_partition)
            {
                // If partition is enabled, actuall we do not need any buddy
                // allocator
                buddy_nr = 1;
            }
            table_ = std::make_shared<HashTableT>(dsm_, buddy_nr);
            // sync providers
            auto providers = table_->providers();
            dsm_->put("size", (uint32_t) providers.size(), 100ms);
            dsm_->put("providers",
                      providers.data(),
                      providers.size() * sizeof(Provider),
                      100ms);
            dsm_->put("race_meta", table_->meta(), 100ms);
        }
        auto size = dsm_->get<uint32_t>("size", 100ms);
        race_meta_ = dsm_->get<GlobalAddress>("race_meta", 100ms);
        providers_.resize(size);
        auto *raw = dsm_->get_raw("providers", 100ms);
        memcpy(providers_.data(), raw, providers_.size() * sizeof(Provider));

        pub_meta_ = dsm_->get<GlobalAddress>("pub", 100ms);
        pub_size_ = dsm_->get<size_t>("pub_size", 100ms);
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }
    bool is_server() const
    {
        return dsm_->get_node_id() == server_nid_;
    }
    bool is_client() const
    {
        return !is_server();
    }
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        // const auto &request_config =
        //     conf.get<bench::RequestGenerator::Config>("request_config");

        // auto [put_rate, get_rate, del_rate] = request_config.put_get_del;
        // if (del_rate != 0)
        // {
        //     avis::Config::ins().configure_drop_deallocated_obj(true);
        //     LOG(WARNING) << "[bench] configure_drop_deallocated_obj(true)";
        // }
        Base::on_start_bench(bls, conf);
    }
    void coro_init(BLS &bls, size_t coro_nr) override
    {
        if (is_client())
        {
            auto &pcls = bls.persistent_coroutine();

            auto ptl = std::make_shared<avis::PTL>(dsm_, server_nid_, nullptr);
            auto pub = std::make_shared<avis::Publisher<avis::BitmapPub>>(
                dsm_, pub_meta_, pub_size_, nullptr);
            auto avis_handle = std::make_shared<avis::AvisHandle>(
                providers_, dsm_, ptl, pub, nullptr);
            pcls.avis_adaptor_ =
                avis::AvisAdaptor::make_ptr(avis_handle, server_nid_);
        }

        Base::coro_init(bls, coro_nr);
    }

    void init_client(BLS &bls, const Config &config, CoroContext *ctx)
    {
        auto &cls = bls.coroutine();
        auto &pcls = bls.persistent_coroutine();
        auto request_config =
            config.get<bench::RequestGenerator::Config>("request_config");
        HashClientT::BenchConfig conf{
            .g = std::make_unique<bench::RequestGenerator>(request_config),
            .en_validate = false};

        cls.handle_ = std::make_shared<HashClientT>(
            dsm_, pcls.avis_adaptor_, race_meta_, conf, server_nid_, ctx);

        // NOTE: must update CoroContext each time
        pcls.avis_adaptor_->set_ctx(ctx);
    }
    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        // token->core().request_stop();

        auto &cls = bls.coroutine();
        // auto &pcls = bls.persistent_coroutine();
        auto &id = bls.id();
        bool is_master = id.is_master_thread() && id.is_master_worker_coro();

        const auto &request_config =
            conf.get<bench::RequestGenerator::Config>("request_config");
        CHECK(request_config.config.limit.has_value());

        const auto limit = request_config.config.limit.value();
        bool verbose = conf.get<bool>("verbose");
        auto [put_rate, get_rate, del_rate] = request_config.config.put_get_del;

        if (is_client())
        {
            init_client(bls, conf, &ctx);
        }

        ChronoTimer timer(is_master /* enable */);

        auto min = std::chrono::nanoseconds(0ns).count();
        auto max = std::chrono::nanoseconds(100ms).count();
        auto step = std::chrono::nanoseconds(1us).count();
        OnePassBucketMonitor<uint64_t> lat_put(min, max, step);
        OnePassBucketMonitor<uint64_t> lat_get(min, max, step);
        OnePassBucketMonitor<uint64_t> lat_del(min, max, step);

        [[maybe_unused]] size_t fulled_nr = 0;
        [[maybe_unused]] size_t not_found_nr = 0;
        size_t ok_nr = 0;

        size_t put_ok = 0;
        size_t del_ok = 0;

        auto now = std::chrono::steady_clock::now();

        auto &op = bls.persistent_coroutine().op_nr;

        for (size_t ith = 0; ith < limit; ++ith)
        {
            if (is_client())
            {
                if (is_master && ith % 100 == 0)
                {
                    auto cur = std::chrono::steady_clock::now();
                    if (cur - now >= 1s)
                    {
                        LOG(INFO)
                            << "[bench] progress: " << ith << " / " << limit
                            << " (" << util::pre_pcnt(1.0 * ith / limit) << ")";
                        now = cur;
                    }
                }

                auto &handle = cls.handle_;
                // auto& avis_handle = pcls.avis_adaptor_;
                op++;
                double select = fast_pseudo_rand_dbl(0, 1);
                bool is_put = select < put_rate;
                bool is_get = select < (put_rate + get_rate);
                bool is_del = true;

                timer.pin();

                if (is_put)
                {
                    auto rc = handle->random_put();
                    auto ns = timer.pin();
                    if (rc == RC::kOk)
                    {
                        if (is_master)
                        {
                            lat_put.collect(ns);
                        }
                        token->complete_task(1);
                        ok_nr++;
                        put_ok++;
                    }
                    // if (rc == RC::kNoMem)
                    // {
                    //     if (fulled_nr++ >= 100)
                    //     {
                    //         LOG(INFO) << "[bench] get " << PRE(rc) << ":
                    //         exit"; token->core().request_stop();
                    //     }
                    // }
                }
                else if (is_get)
                {
                    auto rc = handle->random_get();
                    auto ns = timer.pin();
                    if (is_master)
                    {
                        lat_get.collect(ns);
                    }
                    if (rc == RC::kOk)
                    {
                        ok_nr++;
                        token->complete_task(1);
                    }
                }
                else
                {
                    DCHECK(is_del);
                    auto rc = handle->random_del();
                    auto ns = timer.pin();
                    if (is_master)
                    {
                        lat_del.collect(ns);
                    }
                    if (rc == RC::kOk)
                    {
                        ok_nr++;
                        del_ok++;
                        token->complete_task(1);
                    }
                    // if (rc == RC::kNotFound)
                    // {
                    //     // no need to be too accurate:
                    //     // just large enough to make the table empty.
                    //     if (not_found_nr++ >= kTableCapacity /
                    //     FLAGS_thread_nr)
                    //     {
                    //         LOG(INFO) << "[bench] del " << PRE(rc) << ":
                    //         exit"; token->core().request_stop();
                    //     }
                    // }
                }

                if (ok_nr >= limit)
                {
                    LOG(INFO)
                        << "[bench] " << ok_nr << " / " << limit << " : exit.";
                    break;
                }
            }
        }

        if (is_client() && FLAGS_canon)
        {
            auto &handle = cls.handle_;
            handle->get_handle()->drain();
        }

        if (is_client())
        {
            LOG(INFO) << PRE(put_ok, del_ok);
            if (is_master)
            {
                auto &handle = cls.handle_;
                LOG(INFO) << PRE(lat_put);
                LOG(INFO) << PRE(lat_get);
                LOG(INFO) << PRE(lat_del);
                LOG(INFO) << "Operation: " << PRE(handle->metric());
                auto [_, rm] = handle->get_adaptor()->metrics();
                LOG(INFO) << "Total RDMA: " << rm;
                LOG(INFO) << "PTL induced RDMA: "
                          << handle->get_handle()->ptl()->metric();
                LOG(INFO) << "BP induced RDMA: "
                          << handle->get_handle()->ptl()->bp_metric();
                LOG_IF(INFO, verbose)
                    << "In cache: " << handle->get_handle()->dump();

                if (avis::Config::ins().use_partition_allocator())
                {
                    LOG(INFO) << "Partitions: "
                              << handle->get_adaptor()->partition_metric();
                }

                auto [allc_metrics, rem_metrics] =
                    handle->get_handle()->metrics();
                LOG(INFO) << PRE(rem_metrics);

                LOG(INFO) << "[bench] Mop: " << op / 1e6;
            }
            if (avis::Config::ins().use_partition_allocator())
            {
                auto &handle = cls.handle_;
                LOG(INFO) << "Partitions: "
                          << handle->get_adaptor()->partition_metric();
            }
        }
    }

    void on_end_bench(const ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        bool verbose = conf.get<bool>("verbose");
        static size_t idx = 0;
        idx++;
        if (is_server())
        {
            LOG(INFO) << *table_->table();
            dsm_->put("utilization-" + std::to_string(idx),
                      (double) table_->table()->utilization(),
                      100ms);
            dsm_->put("total_byte-" + std::to_string(idx),
                      (size_t) table_->table()->total_object_bytes(),
                      100ms);
        }
        auto request_config =
            conf.get<bench::RequestGenerator::Config>("request_config");
        double table_util =
            dsm_->get<double>("utilization-" + std::to_string(idx), 100ms);
        size_t total_bytes =
            dsm_->get<size_t>("total_byte-" + std::to_string(idx), 100ms);
        if (is_client())
        {
            auto dumper = std::make_shared<avis::Dumper>(dsm_, providers_);
            dumper->report(verbose);
            auto [used, total] = dumper->bitmap_utilizations();
            LOG(INFO) << "Bitmap Usage: " << util::pre_byte(used) << " / "
                      << util::pre_byte(total) << " (" << used * 100.0 / total
                      << "% )";
            LOG(INFO) << pre_rh_explain<kE, kB, kS>{};

            LOG(INFO) << "Table capacity (maybe): "
                      << util::pre_byte(total_bytes) << " with util "
                      << 100.0 * table_util << "%";
            double client_pcnt = 100.0 * (used - total_bytes) / total_bytes;
            LOG(INFO) << "Client holding: " << util::pre_byte(used) << "(+"
                      << client_pcnt << "%)";
            double bitmap_pcnt = 100.0 * (total - total_bytes) / total_bytes;
            LOG(INFO) << "Bitmap occupies: " << util::pre_byte(total) << "(+"
                      << bitmap_pcnt << "%)";
        }
        Base::on_end_bench(res, bls, conf);
    }

private:
    DSM::pointer dsm_;
    GlobalAddress race_meta_;

    GlobalAddress pub_meta_;
    size_t pub_size_;

    size_t server_nid_;
    std::vector<Provider> providers_;

    // server only
    std::shared_ptr<HashTableT> table_;

    // bench::ResultDataFrame lat_df_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig config;
    config.relaxed_ordering = false;
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
    // config.dsmSize = 100_GB;

    Experiment exp(config);

    ::bench::ConfigFactory f;

    f.configure_thread_nr({FLAGS_thread_nr});
    f.configure_coro_nr({FLAGS_coro_nr});
    // auto request_config =
    //     bench::RequestGenerator::Config::make_default(0.8 /* put rate */,
    //                                                   0.2 /* del rate */,
    //                                                   8 /* key size */,
    //                                                   64 /* value size */,
    //                                                   total_cap /* key rng
    //                                                   */);
    size_t op_nr_per_client = 1.0 * kTableCapacity / (FLAGS_machine_nr - 1) /
                              FLAGS_thread_nr / FLAGS_coro_nr;
    LOG(INFO) << PRE(op_nr_per_client);

    std::vector<std::pair<size_t, double>> etc_workload{
        // ETC workload
        // tiny: (0.4) <= 16
        {8, 0.2},
        {16, 0.2},
        // small (0.59) <= 1400
        // {350, 0.59 / 4},
        // {700, 0.59 / 4},
        // {1050, 0.59 / 4},
        // {1400, 0.59 / 4},
        {233, 0.59 / 6},
        {466, 0.59 / 6},
        {699, 0.59 / 6},
        {932, 0.59 / 6},
        {1165, 0.59 / 6},
        {1398, 0.59 / 6},
        // huge (0.01) <= 1_MB
        {170_KB, 0.01 / 6},
        {340_KB, 0.01 / 6},
        {510_KB, 0.01 / 6},
        {680_KB, 0.01 / 6},
        {850, 0.01 / 6},
        {1_MB, 0.01 / 6},
    };

    bench::RequestGenerator::Config insert_config{
        .config{.put_get_del = {1.0, 0, 0},
                .key_size_dist = {{8, 1.0}},
                .z = 0,
                .key_rng = kTableCapacity,
                .limit = op_nr_per_client * 2

        },
        .value_size_dist = etc_workload,
        .value_size_model = {},
    };

    bench::RequestGenerator::Config del_config{
        .config{
            .put_get_del = {0.5, 0, 0.5},
            .key_size_dist = {{8, 1.0}},
            .z = 0,
            .key_rng = kTableCapacity,
            .limit = (size_t)(1.0 * kTableCapacity * 0.1),
        },
        .value_size_dist = etc_workload,
        .value_size_model = {},
    };

    bench::RequestGenerator::Config small_insert_config{
        .config{
            .put_get_del = {1.0, 0, 0},
            .key_size_dist = {{8, 1.0}},
            .z = 0,
            .key_rng = kTableCapacity,
            .limit = (size_t)(op_nr_per_client / 2),
        },
        .value_size_dist = etc_workload,
        .value_size_model = {},
    };

    bench::RequestGenerator::Config small_mix_config{
        .config{
            .put_get_del = {0.6, 0, 0.4},
            .key_size_dist = {{8, 1.0}},
            .z = 0,
            .key_rng = kTableCapacity,
            .limit = (size_t)(1.0 * kTableCapacity * 0.05),
        },
        .value_size_dist = etc_workload,
        .value_size_model = {},
    };

    bench::RequestGenerator::Config small_mix_del_config{
        .config{
            .put_get_del = {0.4, 0, 0.6},
            .key_size_dist = {{8, 1.0}},
            .z = 0,
            .key_rng = kTableCapacity,
            .limit = (size_t)(1.0 * kTableCapacity * 0.05),
        },
        .value_size_dist = etc_workload,
        .value_size_model = {},
    };
    bench::RequestGenerator::Config small_del_config{
        .config{
            .put_get_del = {0, 0, 1.0},
            .key_size_dist = {{8, 1.0}},
            .z = 0,
            .key_rng = kTableCapacity,
            .limit = (size_t)(1.0 * op_nr_per_client * 0.20),
        },
        .value_size_dist = etc_workload,
        .value_size_model = {},
    };

    if (FLAGS_canon)
    {
        avis::Config::ins().configure_bitmap_size_custom();
    }
    else
    {
        avis::Config::ins().configure_fixed_bitmap_size(FLAGS_block_mb * 1_MB);
        // avis::Config::ins().configure_bitmap_degree(3);
    }

    avis::Config::ins().configure_use_buddy_ge(2_MB);
    avis::Config::ins().configure_share_bitmap(FLAGS_share);

    avis::Config::ins().configure_ptl(false);
    avis::Config::ins().configure_bp(false);
    avis::Config::ins().configure_use_partition_allocator(FLAGS_partition);
    f.add_option<bench::RequestGenerator::Config>("request_config",
                                                  {insert_config});

    // f.add_option<bench::RequestGenerator::Config>("request_config",
    //                                               {insert_config,
    //                                               del_config});
    // f.add_option<bench::RequestGenerator::Config>("request_config",
    //                                               {small_insert_config,
    //                                                small_insert_config,
    //                                                small_insert_config,
    //                                                small_insert_config,
    //                                                small_insert_config,
    //                                                small_mix_del_config,
    //                                                small_mix_del_config,
    //                                                small_mix_del_config,
    //                                                small_mix_del_config});
    // f.add_option<bench::RequestGenerator::Config>("request_config",
    //                                               {small_insert_config,
    //                                                small_insert_config,
    //                                                small_insert_config,
    //                                                small_insert_config,
    //                                                small_insert_config,
    //                                                small_del_config,
    //                                                small_del_config,
    //                                                small_del_config,
    //                                                small_del_config});
    // f.add_option<bench::RequestGenerator::Config>("request_config",
    //                                               {
    //                                                   small_insert_config,
    //                                                   small_insert_config,
    //                                                   small_insert_config,
    //                                                   small_insert_config,
    //                                                   small_insert_config,
    //                                               });
    f.add_option<bool>("verbose", {true});
    // f.add_option<bool>("enable_bp", {false});
    // f.add_option<bool>("enable_ptl", {false});
    // f.add_option<bool>("use_partition", {false});

    exp.configure_monitor(100ms, 100);
    exp.launch_coroutines(f.generate_configs());

    LOG(INFO) << pre_rh_explain<kE, kB, kS>{};

    LOG(INFO) << "PASS.";
}