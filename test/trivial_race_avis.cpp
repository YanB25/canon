#include <numa.h>

#include <thread>

#include "DSM.h"
#include "GlobalAddress.h"
#include "Metrics.h"
#include "avis/AvisAdaptor.h"
#include "avis/avis.h"
#include "avis/bench/hashtable.h"
#include "avis/dump.h"
#include "avis/handle.h"
#include "bench/experiment.h"
#include "bench/request.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "thirdparty/racehashing/hashtable.h"
#include "thirdparty/racehashing/kv_block.h"
#include "thirdparty/racehashing/race.h"
#include "thirdparty/racehashing/utils.h"
#include "util/Rand.h"
#include "util/gflags_def.h"
#include "util/thread_id.h"

using namespace patronus::hash;
using Provider = avis::BuddyProvider;

constexpr static size_t kE = 4;
constexpr static size_t kB = 8;
constexpr static size_t kS = 8;

using HashClientT = HashClient<kE, kB, kS>;
using HashTableT = HashTable<kE, kB, kS>;

class Bench
{
public:
    Bench(DSM::pointer dsm,
          GlobalAddress race_meta,
          size_t key_size,
          size_t value_size,
          CoroContext *ctx)
        : dsm_(dsm), key_size_(key_size), value_size_(value_size)
    {
        uint32_t nid = dsm->get_node_id();
        auto meta_size = 2_MB;
        auto meta = dsm->alloc_from(meta_size, nid, 4_KB);
        auto data_size = 2_GB;
        auto data = dsm->alloc_from(data_size, nid, 4_KB);
        {
            auto rdma_buf = dsm->get_rdma_buffer(meta_size);
            memset(rdma_buf.buffer, 0, meta_size);
            dsm->prepare_write(rdma_buf.buffer, meta, meta_size, false, ctx);
            dsm->commit(ctx);
            dsm->put_rdma_buffer(std::move(rdma_buf));
        }
        auto pub_size = 2_MB;
        auto pub_meta = dsm->alloc_from(pub_size, nid, 4_KB);
        {
            auto rdma_buf = dsm->get_rdma_buffer(pub_size);
            memset(rdma_buf.buffer, 0, pub_size);
            dsm->prepare_write(rdma_buf.buffer, pub_meta, pub_size, false, ctx);
            dsm->commit(ctx);
            dsm->put_rdma_buffer(std::move(rdma_buf));
        }
        providers_.push_back(Provider{.node_id = nid,
                                      .meta_raddr = meta,
                                      .meta_size = 2_MB,
                                      .buf_raddr = data,
                                      .buf_size = 2_GB,
                                      .page_size = 4_KB});
        const auto &default_config =
            bench::RequestGenerator::Config::make_default(
                0.5, 0, key_size_, value_size_, 1024);
        HashClientT::BenchConfig conf{
            .g = std::make_unique<bench::RequestGenerator>(default_config),
            .en_validate = true};

        auto pub = std::make_shared<avis::Publisher<avis::BitmapPub>>(
            dsm_, pub_meta, pub_size, ctx);
        auto avis_handle = std::make_shared<avis::AvisHandle>(
            providers_, dsm_, nullptr /* ptl */, pub, ctx);
        auto avis_adpt = avis::AvisAdaptor::make_ptr(avis_handle, nid);

        c_ = std::make_unique<HashClientT>(
            dsm, avis_adpt, race_meta, conf, nid, ctx);
    }
    auto &client()
    {
        return c_;
    }

    void loop(size_t test_times, double insert_rate, double delete_rate)
    {
        LOG(INFO) << fmt::format(
            "loop({}, {}, {})", test_times, insert_rate, delete_rate);

        for (size_t i = 0; i < test_times; ++i)
        {
            bool is_insert = fast_pseudo_bool_with_prob(insert_rate);
            bool is_delete = fast_pseudo_bool_with_prob(delete_rate);

            if (is_insert)
            {
                c_->random_put();
            }
            else if (is_delete)
            {
                c_->random_del();
            }
            else
            {
                // get
                c_->random_get();
            }
        }

        report();

        reset();

        LOG(INFO) << fmt::format(
            "PASS loop({}, {}, {})", test_times, insert_rate, delete_rate);
    }
    void report()
    {
        LOG(INFO) << c_->metric();
    }
    void reset()
    {
        c_->reset();
    }
    void report_memory(bool verbose = true) const
    {
        std::make_shared<avis::Dumper>(dsm_, providers_)->report(verbose);
    }

private:
    DSM::pointer dsm_;
    size_t key_size_;
    size_t value_size_;

    std::vector<Provider> providers_;
    std::unique_ptr<HashClientT> c_;
};

void playground(DSM::pointer dsm)
{
    HashTableT table(dsm, 1 /* buddy nr */);

    Bench bench(dsm, table.meta(), 8, 64, nullptr);
    // bench.loop(10_K, 0.4, 0.4);
    auto &client = bench.client();

    // LOG(INFO) << "put abc";
    // client->put("abc", "abc");
    // bench.report_memory();
    // LOG(INFO) << "put def";
    // client->put("def", "def");
    // bench.report_memory();
    for (size_t i = 0; i < 1_M; ++i)
    {
        client->random_put();
    }
    bench.report_memory();
    for (size_t i = 0; i < 1_M; ++i)
    {
        client->random_del();
    }

    bench.report_memory();

    // LOG(INFO) << "an insert";
    // client->put("abc22", "def");

    // LOG(INFO) << "an update";
    // client->put("abc", "def");

    // LOG(INFO) << "an delete";
    // client->del("abc");

    // LOG(INFO) << "put ghi";
    // client->put("abc", "ghi");
    // for (size_t i = 0; i < 1024; ++i)
    // {
    //     client->put("abc", "ghi");
    // }
    // for (size_t i = 0; i < 4; ++i)
    // {
    //     LOG(INFO) << "random put";
    //     client->random_put();
    // }
    LOG(INFO) << client->metric();
}

void test(DSM::pointer dsm)
{
    HashTableT table(dsm, 1 /* buddy nr */);
    {
        Bench bench(dsm, table.meta(), 4, 64, nullptr);
        bench.loop(10_K, 0.8, 0);
        bench.loop(10_K, 0.4, 0.4);
        bench.loop(100_K, 0.6, 0.3);
    }
    {
        Bench bench(dsm, table.meta(), 16, 64, nullptr);
        bench.loop(10_K, 0.8, 0);
        bench.loop(10_K, 0.4, 0.4);
        bench.loop(100_K, 0.6, 0.3);
    }
}

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig config;
    config.relaxed_ordering = false;
    auto dsm = DSM::getInstance(config);
    dsm->registerThread();

    // playground(dsm);

    test(dsm);

    LOG(INFO) << "PASS.";
}