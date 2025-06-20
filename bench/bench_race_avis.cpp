#include <chrono>
#include <thread>

#include "Common.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "avis/avis.h"
#include "avis/bench/hashtable.h"
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

// clang-format off
// For running experiment of read-heavy (50get-50put)
// ./bench.py bench_race_avis --block_mb=1 --partition=true --dsm_gb=100 --partition_mb=32
// ./bench.py bench_race_avis --canon  --dsm_gb=100 --ft
// clang-format on

using namespace patronus::hash;

DEFINE_uint64(block_mb, 16, "The size of block in MB");
DEFINE_bool(canon, false, "Whether or not use Canon");
DEFINE_uint32(thread_nr, 32, "The number of thread per machine");
DEFINE_uint32(coro_nr, 3, "The number of coro per thread");
DEFINE_bool(share, false, "Whether or not allow sharing bitmap");
DEFINE_bool(partition, false, "Whether or not use memory partitioning");
DEFINE_uint32(dsm_gb, 40, "The size of DSM in GB");
DEFINE_uint32(cache_gb, 35, "The size of client's RDMA buffers in GB");
DEFINE_uint32(partition_mb,
              512,
              "The size of partition hold by each client in MB");
DEFINE_bool(ft,
            false,
            "Whether or not enable BP + PLS for fault tolerance. This only "
            "work with --canon");

constexpr static size_t kE = 16;
constexpr static size_t kB = 4096;
constexpr static size_t kS = 8;
// constexpr static size_t kE = 64;
// constexpr static size_t kB = 1024 * 8;
// constexpr static size_t kS = 8;

using Provider = avis::BuddyProvider;

// using RaceHashingT = RaceHashing<kE, kB, kS>;
// using HandleT = typename RaceHashingT::Handle;
using TableT = RaceHashing<kE, kB, kS>;

using HashTableT = HashTable<kE, kB, kS>;
using HashClientT = HashClient<kE, kB, kS>;

struct Spec
{
    struct CLS
    {
        std::shared_ptr<HashClientT> handle_;
    };
    struct PCLS
    {
        std::shared_ptr<avis::AvisAdaptor> avis_adaptor_;
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
            table_ = std::make_shared<HashTableT>(dsm_, 4 /* buddy_nr */);
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

    void on_start_bench(BLS &bls, const Config &conf) override
    {
        bool ptl = conf.get<bool>("enable_ptl");
        bool bp = conf.get<bool>("enable_bp");

        avis::Config::ins().configure_ptl(ptl);
        avis::Config::ins().configure_bp(bp);

        Base::on_start_bench(bls, conf);
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
    }
    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        if (unlikely(!is_client()))
        {
            return;
        }

        auto &cls = bls.coroutine();
        auto &id = bls.id();
        bool is_master = id.is_master_thread() && id.is_master_worker_coro();

        const auto &request_config =
            conf.get<bench::RequestGenerator::Config>("request_config");
        bool verbose = conf.get<bool>("verbose");
        auto [put_rate, get_rate, del_rate] = request_config.config.put_get_del;

        init_client(bls, conf, &ctx);

        ChronoTimer timer(is_master /* enable */);

        auto &handle = cls.handle_;

        auto min = std::chrono::nanoseconds(0ns).count();
        auto max = std::chrono::nanoseconds(100ms).count();
        auto step = std::chrono::nanoseconds(1us).count();
        OnePassBucketMonitor<uint64_t> lat_put(min, max, step);
        OnePassBucketMonitor<uint64_t> lat_get(min, max, step);
        OnePassBucketMonitor<uint64_t> lat_del(min, max, step);

        while (!token->stop_requested())
        {
            double select = fast_pseudo_rand_dbl(0, 1);
            bool is_put = select < put_rate;
            bool is_get = select < (put_rate + get_rate);
            bool is_del = true;

            timer.pin();

            if (is_put)
            {
                auto rc = handle->random_put();
                auto ns = timer.pin();
                if (is_master)
                {
                    lat_put.collect(ns);
                }
                if (rc == RC::kOk)
                {
                    token->complete_task(1);
                }
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
                    token->complete_task(1);
                }
            }
        }
        if (is_master)
        {
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

            auto [allc_metrics, rem_metrics] = handle->get_handle()->metrics();
            LOG(INFO) << PRE(rem_metrics);
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
            dsm_->put("total_bytes-" + std::to_string(idx),
                      (uint64_t) table_->table()->total_object_bytes(),
                      100ms);
        }
        double table_util =
            dsm_->get<double>("utilization-" + std::to_string(idx), 100ms);
        auto total_bytes =
            dsm_->get<uint64_t>("total_bytes-" + std::to_string(idx), 100ms);
        if (is_client())
        {
            auto dumper = std::make_shared<avis::Dumper>(dsm_, providers_);
            dumper->report(verbose);
            auto [used, total] = dumper->bitmap_utilizations();
            LOG(INFO) << "Total usage: " << util::pre_byte(used) << " / "
                      << util::pre_byte(total) << " (" << used * 100.0 / total
                      << "% )";
            LOG(INFO) << pre_rh_explain<kE, kB, kS>{};

            // for hash table
            LOG(INFO) << "Table capacity: " << util::pre_byte(total_bytes)
                      << " with util " << 100.0 * table_util << "%";
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

    Experiment exp(config);

    ::bench::ConfigFactory f;

    auto total_cap = TableT::max_capacity();

    // f.configure_thread_nr({1, 2, 4, 8, 12, 16, 20, 24, 28, 32});
    // f.configure_thread_nr({1, 4, 8, 16, 32});
    // f.configure_thread_nr({1, 4, 8});
    // f.configure_thread_nr({1, 4});
    // f.configure_thread_nr({16, 32});
    // f.configure_thread_nr({28, 32});

    // f.configure_thread_nr({32});
    // f.configure_thread_nr({FLAGS_thread_nr});
    // f.configure_coro_nr({FLAGS_coro_nr});
    f.configure_thread_nr({1, 2, 4, 8, 16, 32});
    // f.configure_thread_nr({32});
    f.configure_coro_nr({3});
    // auto request_config =
    //     bench::RequestGenerator::Config::make_default(0.8 /* put rate */,
    //                                                   0.2 /* del rate */,
    //                                                   8 /* key size */,
    //                                                   64 /* value size */,
    //                                                   total_cap /* key rng
    //                                                   */);
    bench::RequestGenerator::Config request_config{
        .config{
            .put_get_del = {0.2, 0.8, 0},
            .key_size_dist = {{8, 1.0}},
            .z = 0,
            .key_rng = total_cap,
        },
        .value_size_dist =
            std::vector<std::pair<size_t, double>>{{16, 0.2},
                                                   {64, 0.2},
                                                   {128 - 25, 0.2},
                                                   {256 - 25, 0.2},
                                                   {512 - 25, 0.2}},
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
    avis::Config::ins().configure_use_buddy_ge(
        std::max(2_MB, FLAGS_block_mb * 1_MB));
    avis::Config::ins().configure_share_bitmap(FLAGS_share);
    avis::Config::ins().configure_use_partition_allocator(FLAGS_partition);
    avis::Config::ins().configure_partition_size(FLAGS_partition_mb * 1_MB);

    f.add_option<bench::RequestGenerator::Config>("request_config",
                                                  {request_config});
    f.add_option<bool>("verbose", {true});
    if (FLAGS_ft)
    {
        f.add_option<bool>("enable_bp", {true});
        f.add_option<bool>("enable_ptl", {true});
        CHECK(FLAGS_canon) << "[param] --ft only work with --canon";
    }
    else
    {
        f.add_option<bool>("enable_bp", {false});
        f.add_option<bool>("enable_ptl", {false});
    }

    exp.configure_monitor(100ms, 40);
    exp.launch_coroutines(f.generate_configs());

    LOG(INFO) << pre_rh_explain<kE, kB, kS>{};

    LOG(INFO) << "PASS.";
}