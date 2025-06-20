#include <city.h>

#include "CoroContext.h"
#include "GlobalAddress.h"
#include "HugePageAlloc.h"
#include "avis/avis.h"
#include "avis/handle.h"
#include "bench/experiment.h"
#include "sherman/Tree.h"
#include "util/Coro.h"
#include "util/Rand.h"
#include "util/concept.h"
#include "util/gflags_def.h"
#include "util/stacktrace.h"

class AvisManager
{
public:
    using Provider = avis::BuddyProvider;
    AvisManager(DSM::pointer dsm, size_t buddy_nr)
        : dsm_(dsm), buddy_nr_(buddy_nr)
    {
        uint32_t nid = dsm->get_node_id();
        for (size_t i = 0; i < buddy_nr_; ++i)
        {
            // This is HashTable, so allocate DSM from me.
            auto meta_size = 2_MB;
            auto meta = dsm_->alloc_from(meta_size, nid, 4_KB);
            CHECK(!meta.is_null());
            auto data_size = 2_GB;
            auto data = dsm_->alloc_from(data_size, nid, 4_KB);
            CHECK(!data.is_null());
            providers_.push_back(Provider{.node_id = nid,
                                          .meta_raddr = meta,
                                          .meta_size = 2_MB,
                                          .buf_raddr = data,
                                          .buf_size = 2_GB,
                                          .page_size = 4_KB});
        }
        dsm_->put("size", (uint32_t) buddy_nr, 100ms);
        dsm_->put("providers",
                  providers_.data(),
                  providers_.size() * sizeof(Provider),
                  100ms);

        auto pub_size = 2_MB;
        auto pub_meta = dsm_->alloc_from(pub_size, nid);
        auto rdma_buf = dsm_->get_rdma_buffer(pub_size);
        memset(rdma_buf.buffer, 0, pub_size);
        dsm_->prepare_write(
            rdma_buf.buffer, pub_meta, pub_size, false, nullptr);
        dsm_->commit();
        dsm_->put_rdma_buffer(std::move(rdma_buf));
        dsm_->put("pub", pub_meta, 100ms);
        dsm_->put("pub_size", pub_size, 100ms);
    }

private:
    DSM::pointer dsm_;
    size_t buddy_nr_;

    std::vector<Provider> providers_;
};

class TreeClient
{
public:
    using Provider = avis::BuddyProvider;
    constexpr static bool kReport = true;
    TreeClient(DSM::pointer dsm,
               GlobalAddress tree_meta,
               size_t server_nid,
               CoroContext *ctx)
        : dsm_(dsm), tree_meta_(tree_meta), server_nid_(server_nid), ctx_(ctx)
    {
        TreeConfig tree_conf;
        tree_conf.allocate_batch_size_ = kInternalPageSize;
        tree_ = sherman::Tree::new_instance(
            CHECK_NOTNULL(dsm), tree_meta, tree_conf, server_nid);
        auto size = dsm_->get<uint32_t>("size", 100ms);
        auto *raw = dsm_->get_raw("providers", 100ms);
        providers_.resize(size);
        memcpy(providers_.data(), raw, size * sizeof(Provider));

        pub_meta_ = dsm_->get<GlobalAddress>("pub", 100ms);
        pub_size_ = dsm_->get<size_t>("pub_size", 100ms);

        auto ptl = std::make_shared<avis::PTL>(dsm_, server_nid_, ctx_);
        auto pub = std::make_shared<avis::Publisher<avis::BitmapPub>>(
            dsm_, pub_meta_, pub_size_, ctx_);

        handle_ = avis::AvisHandle::make_ptr(providers_, dsm_, ptl, pub, ctx_);
    }

    void test(size_t test_nr, size_t key_rng, size_t max_size)
    {
        for (size_t i = 0; i < test_nr; ++i)
        {
            auto key = fast_pseudo_rand_int(0, key_rng - 1);
            auto value_size = fast_pseudo_rand_int(0, max_size);
            LOG_IF(INFO, kReport)
                << "Inserting key " << key << " with size " << value_size;
            insert(key, value_size);
        }
    }

    void playground()
    {
        // insert(10, 20);
        // insert(10, 30);
        // insert(20, 30);
        for (size_t i = 1; i < 1_K; ++i)
        {
            insert(i, i);
        }

        tree_->print_and_check_tree(ctx_);
    }

    void insert(uint64_t key, size_t value_size)
    {
        LOG(INFO) << "Insert " << PRE(key, value_size);
        key = CityHash64((char *) &key, sizeof(key));

        auto raddr = handle_->alloc(value_size);
        size_t total_size = 2 * sizeof(uint64_t) + value_size;
        auto rdma_buf = dsm_->get_rdma_buffer(total_size);
        auto *pu64 = (uint64_t *) rdma_buf.buffer;
        pu64[0] = key;
        pu64[1] = total_size;
        // memcpy(&pu64[2], )
        // TODO: generate value here.
        tree_->prepare_alloc(ctx_);
        tree_->insert(key, raddr.val, ctx_);
    }

private:
    DSM::pointer dsm_;
    GlobalAddress tree_meta_;
    sherman::Tree::pointer tree_;
    size_t server_nid_;
    CoroContext *ctx_;

    std::vector<Provider> providers_;
    GlobalAddress pub_meta_;
    size_t pub_size_;

    std::shared_ptr<avis::AvisHandle> handle_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig config;
    config.worker_nr = 0;
    auto dsm = DSM::getInstance(config);
    dsm->registerThread();

    AvisManager m(dsm, 1 /* buddy nr */);

    auto meta = dsm->alloc(4_KB, 4_KB);

    TreeClient c(dsm, meta, dsm->getClusterSize() - 1, nullptr);
    // c.test(10, 5, 1024);
    c.playground();

    // class TreeClient
    // {
    // public:
    //     using Provider = avis::BuddyProvider;
    //     constexpr static bool kReport = true;
    //     TreeClient(DSM::pointer dsm,
    //                sherman::Tree::pointer tree,
    //                size_t server_nid,
    //                CoroContext *ctx)

    //         return 0;
    // }
    return 0;
}
