#include <fcntl.h>
#include <infiniband/verbs_exp.h>
#include <numa.h>
#include <sys/mman.h>
#include <unistd.h>

#include <filesystem>
#include <thread>
#include <type_traits>

#include "GlobalAddress.h"
#include "HugePageAlloc.h"
#include "Rdma.h"
#include "Timer.h"
#include "avis/AvisAdaptor.h"
#include "avis/avis.h"
#include "avis/buddy.h"
#include "avis/config.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "jemalloc/jemalloc.h"
#include "jemalloc_cpp/jemalloc_cpp.h"
#include "memory/asan_interfaces.h"
#include "patronus/memory/direct_allocator.h"
#include "thirdparty/racehashing/hashtable.h"
#include "thirdparty/racehashing/race.h"
#include "util/Bitset.h"
#include "util/Hexdump.hpp"
#include "util/ProcessMem.h"
#include "util/Rand.h"
#include "util/System.h"
#include "util/Util.h"
#include "util/bits.h"
#include "util/concept.h"
#include "util/gflags_def.h"
#include "util/stacktrace.h"

void test(DSM::pointer dsm)
{
    auto rdma_buf = dsm->get_rdma_buffer(32);
    auto gaddr = dsm->alloc(32);
    memset(rdma_buf.buffer, 0xff, 32);

    dsm->prepare_write(rdma_buf.buffer, gaddr, 32, false, nullptr);
    dsm->commit();

    char add_val[32];
    memset(add_val, 0xff, sizeof(add_val));
    char field_boundary[32];
    memset(field_boundary, 0xff, sizeof(field_boundary));
    dsm->prepare_faa(gaddr,
                     32,
                     (uint64_t) add_val,
                     (uint64_t) field_boundary,
                     rdma_buf.buffer,
                     false);
    dsm->commit();
    LOG(INFO) << "After FAA, rdma_buf: " << util::Hexdump(rdma_buf.buffer, 32);
    auto [nid, buf] = dsm->explain_gaddr(gaddr);
    CHECK_EQ(nid, dsm->get_node_id());
    LOG(INFO) << "After FAA, local_buf: " << util::Hexdump(buf, 32);
}

void cas_get(DSM::pointer dsm, GlobalAddress gaddr, size_t size)
{
    auto rdma_buf = dsm->get_rdma_buffer(size);
    std::vector<char> compare(size, 0);
    std::vector<char> swap(size, 0xff);
    std::vector<char> mask(size, 0xff);

    dsm->prepare_cas(gaddr,
                     size,
                     (uint64_t) compare.data(),
                     (uint64_t) mask.data(),
                     (uint64_t) swap.data(),
                     (uint64_t) mask.data(),
                     rdma_buf.buffer,
                     false,
                     nullptr);
    dsm->commit();

    LOG(INFO) << "[CAS] get_old: " << std::endl
              << util::Bindump(rdma_buf.buffer, size);
    uint64_t *tmp = (uint64_t *) malloc(size);
    memcpy(tmp, rdma_buf.buffer, size);
    for (size_t i = 0; i < size / sizeof(uint64_t); ++i)
    {
        tmp[i] = bswap64(tmp[i]);
    }
    LOG(INFO) << "[CAS] after htonll got: " << std::endl
              << util::Bindump(tmp, size);
    free(tmp);
    dsm->put_rdma_buffer(std::move(rdma_buf));
}

void faa_get(DSM::pointer dsm, GlobalAddress gaddr, size_t size)
{
    auto rdma_buf = dsm->get_rdma_buffer(size);
    std::vector<char> add_val(size, 0);
    std::vector<char> boundary(size, 0xff);

    dsm->prepare_faa(gaddr,
                     size,
                     (uint64_t) add_val.data(),
                     (uint64_t) boundary.data(),
                     rdma_buf.buffer,
                     false,
                     nullptr);
    dsm->commit();

    auto bs = util::BitsViewMut(rdma_buf.buffer, size);
    LOG(INFO) << "[FAA] get_old: " << std::endl << bs.bin_dump();
    bs.bswap64();
    LOG(INFO) << "[FAA] get_old after bswap64: " << std::endl << bs.bin_dump();

    dsm->put_rdma_buffer(std::move(rdma_buf));
}

class Test
{
public:
    bool kReport = false;
    Test(DSM::pointer dsm, size_t page_size, size_t total_size)
        : dsm_(dsm),
          page_size_(page_size),
          total_size_(total_size),
          page_nr_(total_size / page_size)
    {
        data_ = dsm_->alloc(total_size, page_size /* alignment */);
        meta_ = dsm_->alloc(2_MB);

        rdma_buf_ = dsm_->get_rdma_buffer(page_size);
        memset(rdma_buf_.buffer, 0, rdma_buf_.size);

        auto ptl =
            std::make_shared<avis::PTL>(dsm, dsm_->get_node_id(), nullptr);

        LOG(INFO) << PRE(meta_, data_);
        buddy_ = std::make_unique<avis::BuddyAllocator>(dsm_,
                                                        meta_,
                                                        2_MB /* meta size */,
                                                        data_,
                                                        total_size,
                                                        page_size,
                                                        ptl,
                                                        nullptr);
        LOG(INFO) << PRE(*buddy_);

        // clearing meta
        auto meta_buf = dsm_->get_rdma_buffer(2_MB);
        memset(meta_buf.buffer, 0, 2_MB);
        dsm_->prepare_write(meta_buf.buffer, meta_, 2_MB, false, nullptr);
        dsm_->commit();
        dsm_->put_rdma_buffer(std::move(meta_buf));
    }
    ~Test()
    {
        dsm_->free(data_, total_size_);
        dsm_->free(meta_, 2_MB);
        dsm_->put_rdma_buffer(std::move(rdma_buf_));
    }

    void playground()
    {
        // buddy_->report_fragmentation();
        // buddy_->get_free_pages(0);
        // buddy_->report_fragmentation();
        // buddy_->get_free_pages(0);
        // buddy_->report_fragmentation();

        // {
        //     LOG(INFO) << "get_free_pages(0)";
        //     buddy_->get_free_pages(0);
        //     auto [_, rm] = buddy_->metrics();
        //     LOG(INFO) << PRE(rm);
        //     buddy_->metric_reset();
        // }
        // {
        //     LOG(INFO) << "get_free_pages(1)";
        //     buddy_->get_free_pages(1);
        //     auto [_, rm] = buddy_->metrics();
        //     LOG(INFO) << PRE(rm);
        //     buddy_->metric_reset();
        // }
        // {
        //     LOG(INFO) << "get_free_pages(2)";
        //     buddy_->get_free_pages(2);
        //     auto [_, rm] = buddy_->metrics();
        //     LOG(INFO) << PRE(rm);
        //     buddy_->metric_reset();
        // }
        for (int order = 0; order <= 10; ++order)
        {
            buddy_->metric_reset();
            LOG(INFO) << fmt::format("get_free_pages({})", order);
            buddy_->get_free_pages(order);
            auto [_, rm] = buddy_->metrics();
            LOG(INFO) << PRE(rm);
        }
    }

    void run()
    {
        hello();
        reset();

        alloc_all_free_all(0);
        reset();

        alloc_all_free_all(1);
        reset();

        alloc_all_free_all(4);
        reset();

        random_alloc_free(100, 0, 4);
        reset();

        random_alloc_free(10_K, 0, 4);
        reset();

        random_alloc_free(10_K, 0, buddy_->max_order());
        reset();
    }

    void hello()
    {
        LOG(INFO) << "hello()";
        auto addr = alloc(0);
        CHECK(!addr.is_null());
        dealloc(addr, 0);
        LOG(INFO) << "PASS hello";
    }
    void random_alloc_free(size_t test_nr,
                           unsigned min_order,
                           unsigned max_order)
    {
        LOG(INFO) << fmt::format(
            "random_alloc_free({}, {}, {})", test_nr, min_order, max_order);
        bool alloc_phrase = true;
        size_t allocated_page = 0;
        for (size_t i = 0; i < test_nr; ++i)
        {
            if (allocated_.empty())
            {
                alloc_phrase = true;
            }
            if (alloc_phrase)
            {
                auto order = fast_pseudo_rand_int(min_order, max_order);
                auto addr = alloc(order);
                if (addr.is_null())
                {
                    CHECK_LE(allocated_page, page_nr_);
                    alloc_phrase = false;
                }
                else
                {
                    allocated_page += pow(2, order);
                    CHECK_LE(allocated_page, page_nr_);
                }
            }
            else
            {
                CHECK(!allocated_.empty());
                auto [addr, order] = *allocated_.begin();
                CHECK(!addr.is_null());
                dealloc(addr, order);
                auto page_nr = 1ull << order;
                CHECK_GE(allocated_page, page_nr);
                allocated_page -= page_nr;
            }
        }
        LOG(INFO) << fmt::format("PASS random_alloc_free({}, {}, {})",
                                 test_nr,
                                 min_order,
                                 max_order);
    }
    void alloc_all_free_all(unsigned order)
    {
        while (true)
        {
            auto addr = alloc(order);
            if (addr.is_null())
            {
                break;
            }
        }
        auto [alloc, err] = buddy_->get_allocated();
        CHECK(err.empty());

        CHECK_EQ(alloc.size() * pow(2, order), page_nr_)
            << "allocated " << alloc.size() << ", each has " << pow(2, order)
            << " pages with order " << order
            << " but not equal to page_nr: " << page_nr_ << std::endl
            << PRE(alloc);

        size_t times = 0;
        auto max_times = alloc.size();
        while (!allocated_.empty())
        {
            auto [addr, order] = *allocated_.begin();
            dealloc(addr, order);
            times++;
            CHECK_LT(times, 2 * max_times) << "** run forever";
        }
        LOG(INFO) << "PASS alloc_all_free_all " << order;
    }

    void reset()
    {
        memset(rdma_buf_.buffer, 0, rdma_buf_.size);
        dsm_->prepare_write(
            rdma_buf_.buffer, meta_, page_size_, false, nullptr);
        dsm_->commit();
        allocated_.clear();
        buddy_->reset();
        auto [alloc, error] = buddy_->get_allocated();
        CHECK(error.empty());
        CHECK(alloc.empty());
    }

    GlobalAddress alloc(unsigned int order, bool do_check = true)
    {
        auto addr = buddy_->get_free_pages(order);
        LOG_IF(INFO, kReport) << "[alloc] " << PRE(addr) << " order " << order;
        if (addr.is_null())
        {
            return addr;
        }

        CHECK(allocated_.emplace(addr, order).second)
            << "** double allocation: " << PRE(addr, allocated_);

        if (do_check)
        {
            check();
        }
        return addr;
    }
    void dealloc(GlobalAddress addr, unsigned order, bool do_check = true)
    {
        CHECK_EQ(allocated_.erase(addr), 1)
            << "** addr " << addr << " not allocated";
        LOG_IF(INFO, kReport) << "[free] " << PRE(addr);
        buddy_->put_free_pages(addr, order);

        if (do_check)
        {
            check();
        }
    }

    void check()
    {
        auto [alloc, err] = buddy_->get_allocated();
        CHECK(err.empty()) << "** " << PRE(err);
        if (alloc.size() != allocated_.size())
        {
            LOG(ERROR) << "Expect " << alloc.size()
                       << ", got: " << allocated_.size() << std::endl
                       << PRE(alloc) << std::endl
                       << PRE(allocated_);
            buddy_->print_meta();
            LOG(FATAL) << "Die.";
        }
    }

private:
    DSM::pointer dsm_;
    GlobalAddress data_;
    GlobalAddress meta_;
    Buffer rdma_buf_;
    std::unique_ptr<avis::BuddyAllocator> buddy_;

    size_t page_size_;
    [[maybe_unused]] size_t total_size_;
    size_t page_nr_;

    std::map<GlobalAddress, unsigned> allocated_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig config;
    config.worker_nr = 0;

    auto dsm = DSM::getInstance(config);
    dsm->registerThread();

    {
        LOG(INFO) << "Testing 4_KB, 2_MB";
        Test test(dsm, 4_KB, 2_MB);
        test.run();
    }

    {
        LOG(INFO) << "Testing 2_MB, 2_GB";
        Test test(dsm, 2_MB, 2_GB);
        test.run();
    }

    // {
    //     avis::Config::ins().configure_buddy_mode(avis::BuddyMode::kRand);
    //     avis::Config::ins().configure_buddy_use_post_order(true);
    //     // avis::Config::ins().configure_buddy_use_post_order(true);
    //     Test test(dsm, 4_KB, 2_GB);
    //     test.playground();
    // }

    LOG(INFO) << "PASS";
    return 0;
}