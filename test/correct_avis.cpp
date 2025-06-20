#include "DSM.h"
#include "DSMCache.h"
#include "GlobalAddress.h"
#include "GlobalAllocator.h"
#include "avis/AvisAdaptor.h"
#include "avis/avis.h"
#include "avis/buddy.h"
#include "avis/dump.h"
#include "avis/handle.h"
#include "avis/publisher.h"
#include "avis/slab_cache.h"
#include "bench/experiment.h"
#include "glog/logging.h"
#include "util/Rand.h"
#include "util/gflags_def.h"

using Provider = avis::BuddyProvider;

class Handle
{
public:
    Handle(DSM::pointer dsm,
           GlobalAddress meta,
           size_t meta_size,
           GlobalAddress data,
           size_t data_size)
        : dsm_(dsm)
    {
        auto rdma_buf = dsm_->get_rdma_buffer(meta_size);
        memset(rdma_buf.buffer, 0, meta_size);
        dsm_->prepare_write(rdma_buf.buffer, meta, meta_size, false, nullptr);
        dsm_->commit();
        dsm_->put_rdma_buffer(std::move(rdma_buf));

        uint32_t nid = dsm_->get_node_id();
        providers_.push_back(Provider{.node_id = nid,
                                      .meta_raddr = meta,
                                      .meta_size = meta_size,
                                      .buf_raddr = data,
                                      .buf_size = data_size,
                                      .page_size = 4_KB});

        auto pub_size = 2_MB;
        auto pub_meta = dsm_->alloc(pub_size);
        auto rdma_buf2 = dsm_->get_rdma_buffer(pub_size);
        memset(rdma_buf2.buffer, 0, pub_size);
        dsm_->prepare_write(
            rdma_buf2.buffer, pub_meta, pub_size, false, nullptr);
        dsm_->commit();
        dsm_->put_rdma_buffer(std::move(rdma_buf2));
        auto pub = std::make_shared<avis::Publisher<avis::BitmapPub>>(
            dsm_, pub_meta, pub_size, nullptr);

        auto ptl = std::make_shared<avis::PTL>(dsm_, nid, nullptr);
        handle_ = std::make_shared<avis::AvisHandle>(
            providers_, dsm_, ptl, pub, nullptr);
        locator_ = std::make_shared<avis::Locator>(providers_);
    }

    void playground()
    {
        LOG(INFO) << "playground()";

        // LOG(WARNING) << "alloc 64";
        // handle_->alloc(64_B);
        // report_memory();
        // LOG(WARNING) << "alloc 128";
        // handle_->alloc(128_B);
        // report_memory();
        // LOG(WARNING) << "alloc 256";
        // handle_->alloc(256_B);
        // report_memory();
        for (size_t i = 1; i < 1_KB; ++i)
        {
            handle_->alloc(i);
        }
        // handle_->alloc(32_KB);
        report_memory();
    }
    void report_memory()
    {
        std::make_shared<avis::Dumper>(dsm_, providers_)->report();
    }

    void hello()
    {
        LOG(INFO) << "hello()";

        auto raddr = alloc(64);
        CHECK(!raddr.is_null());
        LOG(INFO) << PRE(raddr);
        auto raddr2 = alloc(4_KB);
        CHECK(!raddr2.is_null());
        LOG(INFO) << PRE(raddr2);
        auto raddr3 = alloc(16_KB);
        CHECK(!raddr3.is_null());
        LOG(INFO) << PRE(raddr3);
        auto raddr4 = alloc(64);
        CHECK(!raddr4.is_null());
        LOG(INFO) << PRE(raddr4);

        handle_->report();

        free(raddr, 64);
        free(raddr2, 4_KB);
        free(raddr3, 16_KB);
        free(raddr4, 64);

        handle_->report();

        LOG(INFO) << "END hello()";
    }

    void loop(size_t loop_nr,
              size_t op_nr,
              const std::vector<size_t> &size_class)
    {
        LOG(INFO) << fmt::format(
            "loop({}, {}, {})", loop_nr, op_nr, util::pre(size_class));
        for (size_t l = 0; l < loop_nr; ++l)
        {
            for (size_t i = 0; i < op_nr; ++i)
            {
                auto size =
                    size_class[fast_pseudo_rand_int() % size_class.size()];
                auto raddr = alloc(size);
                CHECK(!raddr.is_null());
            }

            // report_memory();
            validate();
            free_all();
        }
        LOG(INFO) << "END loop";
    }

    void free_all(bool drain = true)
    {
        for (const auto &[size, set] : alloced_)
        {
            for (auto raddr : set)
            {
                free(raddr, size);
            }
        }
        alloced_.clear();
        if (drain)
        {
            // LOG(WARNING) << "drain";
            handle_->drain();
        }
    }

    void loop_random(size_t loop_nr,
                     size_t op_nr,
                     size_t min_size,
                     size_t max_size)
    {
        LOG(INFO) << fmt::format(
            "loop_random(loop_nr={}, op_nr={}, min_size={}, max_size={})",
            loop_nr,
            op_nr,
            min_size,
            max_size);
        for (size_t l = 0; l < loop_nr; ++l)
        {
            for (size_t i = 0; i < op_nr; ++i)
            {
                auto size = fast_pseudo_rand_int(min_size, max_size);
                auto raddr = alloc(size);
                CHECK(!raddr.is_null());
            }

            report_memory();
            validate();
            free_all();
        }
        LOG(INFO) << fmt::format(
            "END loop_random(loop_nr={}, op_nr={}, min_size={}, max_size={})",
            loop_nr,
            op_nr,
            min_size,
            max_size);
    }

    void report()
    {
        handle_->report();
    }

    GlobalAddress alloc(size_t size)
    {
        auto raddr = handle_->alloc(size);
        CHECK(!raddr.is_null());
        alloced_[size].insert(raddr);
        return raddr;
    }
    void free(GlobalAddress raddr, size_t size)
    {
        CHECK(!raddr.is_null());
        auto &set = alloced_[size];
        auto it = set.find(raddr);
        if (it == set.end())
        {
            LOG(FATAL) << "freeing not allocated: " << PRE(raddr, size);
        }
        set.erase(it);

        handle_->free(raddr, size);
    }

    void validate()
    {
        // LOG(INFO) << "validating...";
        std::vector<Buffer> buffers;
        for (const auto &[size, set] : alloced_)
        {
            for (auto raddr : set)
            {
                buffers.emplace_back(Buffer((char *) raddr.offset, size));
            }
        }
        validate_buffer_not_overlapped(buffers);
        // LOG(INFO) << "PASS";
    }
    auto inner()
    {
        return handle_;
    }

protected:
    DSM::pointer dsm_;
    std::vector<Provider> providers_;
    std::shared_ptr<avis::AvisHandle> handle_;
    std::shared_ptr<avis::Locator> locator_;

    // internal management
    std::map<size_t, std::set<GlobalAddress>> alloced_;
};

class CrossClient
{
public:
    constexpr static bool kReport = true;
    CrossClient(DSM::pointer dsm,
                size_t client_nr,
                GlobalAddress meta,
                size_t meta_size,
                GlobalAddress data,
                size_t data_size,
                GlobalAddress pub_meta,
                size_t pub_size)
        : dsm_(dsm), pub_meta_(pub_meta), pub_size_(pub_size)
    {
        uint32_t nid = dsm_->get_node_id();

        auto rdma_buf = dsm_->get_rdma_buffer(meta_size);
        memset(rdma_buf.buffer, 0, meta_size);
        dsm_->prepare_write(rdma_buf.buffer, meta, meta_size, false, nullptr);
        dsm_->commit();
        dsm_->put_rdma_buffer(std::move(rdma_buf));

        providers_.push_back(Provider{.node_id = nid,
                                      .meta_raddr = meta,
                                      .meta_size = meta_size,
                                      .buf_raddr = data,
                                      .buf_size = data_size,
                                      .page_size = 4_KB});

        for (size_t i = 0; i < client_nr; ++i)
        {
            auto ptl = std::make_shared<avis::PTL>(dsm_, nid, nullptr);
            auto pub = std::make_shared<avis::Publisher<avis::BitmapPub>>(
                dsm_, pub_meta_, pub_size_, nullptr);
            pub->init();
            handles_.emplace_back(std::make_shared<avis::AvisHandle>(
                providers_, dsm_, ptl, pub, nullptr));
        }
        locator_ = std::make_shared<avis::Locator>(providers_);
    }

    void hello()
    {
        auto &h1 = handle(0);
        auto &h2 = handle(1);

        auto addr = h1->alloc(64);
        LOG(INFO) << PRE(addr);
        h2->free(addr, 64);
        h2->drain();

        reset();
    }
    void playgroud()
    {
        auto &h1 = handle(0);
        auto &h2 = handle(1);

        auto addr1 = h1->alloc(64);
        auto addr2 = h2->alloc(64);
        LOG(INFO) << "Allocted 2";
        LOG(INFO) << PRE(h1->dump(), h2->dump());
        h2->free(addr1, 64);
        h1->free(addr2, 64);
        LOG(INFO) << "freed 2";
        LOG(INFO) << PRE(h1->dump(), h2->dump());
        h1->drain();
        h2->drain();
        LOG(INFO) << "drained";
        LOG(INFO) << PRE(h1->dump(), h2->dump());
    }

    void test(size_t test_nr, size_t alloc_nr, size_t alloc_size)
    {
        LOG(INFO) << fmt::format(
            "test({}, {}, {})", test_nr, alloc_nr, alloc_size);

        for (size_t t = 0; t < test_nr; ++t)
        {
            std::vector<GlobalAddress> addrs;
            addrs.reserve(alloc_nr);
            for (size_t i = 0; i < alloc_nr; ++i)
            {
                auto id = fast_pseudo_rand_int(0, handles_.size() - 1);

                auto addr = alloc(id, alloc_size);
                addrs.emplace_back(addr);
            }
            // dump_all();
            for (const auto &addr : addrs)
            {
                auto id = fast_pseudo_rand_int(0, handles_.size() - 1);
                free(id, addr, alloc_size);
            }
            reset();
        }

        reset();
        LOG(INFO) << fmt::format(
            "PASS test({}, {}, {})", test_nr, alloc_nr, alloc_size);
    }

private:
    DSM::pointer dsm_;
    std::vector<Provider> providers_;

    GlobalAddress pub_meta_;
    size_t pub_size_;

    std::vector<std::shared_ptr<avis::AvisHandle>> handles_;
    std::shared_ptr<avis::Locator> locator_;

    // std::vector<std::pair<GlobalAddress, size_t>> alloced_;
    std::unordered_map<GlobalAddress, size_t> alloced_;

    void dump_all()
    {
        for (size_t i = 0; i < handles_.size(); ++i)
        {
            auto &handle = handles_[i];
            LOG(INFO) << i << ": " << handle->dump();
        }
    }

    void validate()
    {
        std::vector<Buffer> buffers;
        for (const auto &[raddr, size] : alloced_)
        {
            buffers.emplace_back(Buffer((char *) raddr.offset, size));
        }
        validate_buffer_not_overlapped(buffers);
    }

    std::shared_ptr<avis::AvisHandle> &handle(size_t i)
    {
        return handles_[i];
    }

    void reset()
    {
        for (const auto &[addr, size] : alloced_)
        {
            free(0, addr, size);
        }
        alloced_.clear();

        for (auto &h : handles_)
        {
            h->drain();
        }
    }

    GlobalAddress alloc(size_t id, size_t size)
    {
        auto &h = handle(id);
        auto addr = h->alloc(size);
        if (addr.is_null())
        {
            LOG(ERROR) << "Run out of memory";
            std::make_shared<avis::Dumper>(dsm_, providers_)->report(true);
            LOG(FATAL) << "Die: " << PRE(id, size);
        }
        CHECK(!addr.is_null());
        // LOG_IF(INFO, kReport) << "===== alloc " << addr;
        alloced_.emplace(addr, size);
        return addr;
    }

    void free(size_t id, GlobalAddress raddr, size_t size)
    {
        auto &h = handle(id);

        CHECK_EQ(alloced_.erase(raddr), 1);
        // LOG_IF(INFO, kReport) << "===== freeing " << raddr;
        CHECK(!raddr.is_null());
        h->free(raddr, size);
    }
};

class Adaptor : public Handle
{
public:
    Adaptor(DSM::pointer dsm,
            GlobalAddress meta,
            size_t meta_size,
            GlobalAddress data,
            size_t data_size)
        : Handle(dsm, meta, meta_size, data, data_size)
    {
        adpt_ = avis::AvisAdaptor::make_ptr(handle_, dsm->get_node_id());
    }
    void hello()
    {
        h_ = adpt_->acquire_perm(GlobalAddress::Null(),
                                 0,
                                 std::numeric_limits<size_t>::max(),
                                 1ns,
                                 0);

        DCHECK(h_.valid());

        auto raddr = adpt_->remote_alloc(64, 0);
        auto rdma_buf = adpt_->get_rdma_buffer(64);
        memset(rdma_buf.buffer, 'a', 64);
        adpt_->rdma_write(raddr, rdma_buf.buffer, 64, 0, h_).expect(RC::kOk);
        adpt_->commit().expect(RC::kOk);

        auto rdma_buf2 = adpt_->get_rdma_buffer(64);
        adpt_->rdma_read(rdma_buf2.buffer, raddr, 64, 0, h_).expect(RC::kOk);
        adpt_->commit().expect(RC::kOk);
        for (size_t i = 0; i < 64; ++i)
        {
            CHECK_EQ(rdma_buf2.buffer[i], 'a');
        }

        adpt_->put_all_rdma_buffer();
    }

private:
    avis::AvisAdaptor::Pointer adpt_;
    RemoteMemHandle h_;
};

void test_avis_handle(DSM::pointer dsm,
                      GlobalAddress meta,
                      size_t meta_size,
                      GlobalAddress data,
                      size_t data_size)
{
    Handle h(dsm, meta, meta_size, data, data_size);
    // h.playground();

    h.hello();
    h.loop(1, 10, {64, 128, 256});
    h.loop(1, 1_K, {64, 128, 256});
    h.loop(1, 10_K, {64, 128, 256});

    h.loop(10, 1_K, {64, 128, 256});
    h.loop(50, 1_K, {64, 128, 256});

    h.loop_random(1, 10, 1, 4_KB);
    h.loop_random(1, 1_K, 1, 4_KB);
    h.loop_random(1, 10_K, 1, 4_KB);

    h.loop_random(10, 1_K, 1, 4_KB);
    h.loop_random(50, 1_K, 1, 4_KB);
}

void test_avis_adaptor(DSM::pointer dsm,
                       GlobalAddress meta,
                       size_t meta_size,
                       GlobalAddress data,
                       size_t data_size)
{
    Adaptor adpt(dsm, meta, meta_size, data, data_size);
    adpt.hello();
}

void test_avis_cross_client(DSM::pointer dsm,
                            GlobalAddress meta,
                            size_t meta_size,
                            GlobalAddress data,
                            size_t data_size,
                            GlobalAddress pub_meta,
                            size_t pub_size)
{
    {
        CrossClient cc(dsm,
                       2 /* client_nr */,
                       meta,
                       meta_size,
                       data,
                       data_size,
                       pub_meta,
                       pub_size);

        cc.test(1, 10, 64);

        cc.test(1, 100, 64);

        cc.test(10_K, 100, 64);

        cc.test(1, 100, 32_KB);

        // below for huge pages
        cc.test(1, 1_K, 64_KB);
        cc.test(100, 1_K, 64_KB);
    }

    {
        CrossClient cc(dsm,
                       8 /* client_nr */,
                       meta,
                       meta_size,
                       data,
                       data_size,
                       pub_meta,
                       pub_size);
        cc.test(1_K, 100, 64);

        cc.test(40, 4_K, 64_KB);
    }
}

void playground(DSM::pointer dsm,
                GlobalAddress meta,
                size_t meta_size,
                GlobalAddress data,
                size_t data_size)
{
    Handle h(dsm, meta, meta_size, data, data_size);
    h.playground();

    // CrossClient cc(dsm, 2 /* client_nr */, meta, meta_size, data, data_size);
    // cc.playgroud();
}

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig config;
    config.relaxed_ordering = false;
    auto dsm = DSM::getInstance(config);
    dsm->registerThread();

    uint32_t nid = dsm->get_node_id();
    auto meta_size = 2_MB;
    auto meta = dsm->alloc_from(meta_size, nid, 4_KB);
    auto data_size = 2_GB;
    auto data = dsm->alloc_from(data_size, nid, 4_KB);
    auto pub_size = 2_MB;
    auto pub_meta = dsm->alloc_from(pub_size, nid, 4_KB);

    // playground(dsm, meta, meta_size, data, data_size);

    // avis::Config::ins().configure_bitmap_degree(-4);

    test_avis_handle(dsm, meta, meta_size, data, data_size);

    test_avis_adaptor(dsm, meta, meta_size, data, data_size);

    test_avis_cross_client(
        dsm, meta, meta_size, data, data_size, pub_meta, pub_size);

    return 0;
}
