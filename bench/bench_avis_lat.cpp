#include <numa.h>

#include <chrono>
#include <thread>
#include <vector>

#include "Timer.h"
#include "avis/avis.h"
#include "avis/bitmap.h"
#include "avis/buddy.h"
#include "avis/policy.h"
#include "avis/ptl.h"
#include "avis/slab_cache.h"
#include "bench/DataFrame.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Bitset.h"
#include "util/PerformanceReporter.h"
#include "util/gflags_def.h"

class Handle
{
public:
    Handle(DSM::pointer dsm) : dsm_(dsm)
    {
        uint32_t nid = dsm_->getClusterSize() - 1;
        meta_size_ = 2_MB;
        meta_ = dsm_->alloc_from(meta_size_, nid, 4_KB);
        data_size_ = 2_GB;
        data_ = dsm_->alloc_from(data_size_, nid, 4_KB);

        auto ptl = std::make_shared<avis::PTL>(dsm_, nid, nullptr);
        buddy_ = std::make_shared<avis::BuddyAllocator>(
            dsm_, meta_, meta_size_, data_, data_size_, 4_KB, ptl, nullptr);

        auto bm_data_addr = buddy_->get_free_pages(4);
        auto bm_data_size = (1ull << 4) * 4_KB;
        bitmap_ = std::make_shared<avis::BitmapSlab>(
            dsm_, bm_data_addr, bm_data_size, 64, ptl, nullptr);
        bitmap_->init();

        CHECK(!bitmap_->empty());

        cache_ = std::make_shared<avis::SlabCache>(bitmap_, dsm_, ptl, nullptr);
    }

    bool is_client() const
    {
        return dsm_->get_node_id() == 0;
    }

    void bench()
    {
        if (is_client())
        {
            buddy_latency();
            slab_latency(64);
            cache_latency(256);
        }
    }

    void cache_latency(size_t times)
    {
        bitmap_->reset();
        bitmap_->init();

        auto min = std::chrono::nanoseconds(0ns).count();
        auto max = std::chrono::nanoseconds(6us).count();
        auto rng = std::chrono::nanoseconds(10ns).count();
        OnePassBucketMonitor<uint64_t> alloc_lat_m(min, max, rng);

        ChronoTimer timer;
        for (size_t i = 0; i < times; ++i)
        {
            timer.pin();
            cache_->alloc(64);
            auto alloc_ns = timer.pin();
            alloc_lat_m.collect(alloc_ns);
        }
        LOG(INFO) << "cache_latency: " << PRE(alloc_lat_m);
    }

    void slab_latency(size_t times)
    {
        auto min = std::chrono::nanoseconds(0ns).count();
        auto max = std::chrono::nanoseconds(7us).count();
        auto rng = std::chrono::nanoseconds(100ns).count();
        OnePassBucketMonitor<uint64_t> alloc_lat_m(min, max, rng);

        ChronoTimer timer;
        for (size_t i = 0; i < times; ++i)
        {
            std::vector<GlobalAddress> vec;
            vec.reserve(1024);

            timer.pin();
            bitmap_->alloc(vec, 1 /* block_nr */, false /* read meta */);
            auto alloc_ns = timer.pin();
            alloc_lat_m.collect(alloc_ns);
        }

        LOG(INFO) << "slab_latency: " << PRE(alloc_lat_m);
    }

    void buddy_latency()
    {
        bench::ResultDataFrame df;
        for (unsigned order : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10})
        {
            do_buddy_latency(order, 64, df);
        }
        df.print();
    }

    void do_buddy_latency(unsigned order,
                          size_t times,
                          bench::ResultDataFrame &df)
    {
        auto min = std::chrono::nanoseconds(0ns).count();
        auto max = std::chrono::nanoseconds(10us).count();
        auto rng = std::chrono::nanoseconds(1us).count();
        OnePassBucketMonitor<uint64_t> alloc_lat_m(min, max, rng);
        OnePassBucketMonitor<uint64_t> free_lat_m(min, max, rng);

        ChronoTimer timer;
        for (size_t i = 0; i < times; ++i)
        {
            timer.pin();
            auto addr = buddy_->get_free_pages(order);
            auto alloc_ns = timer.pin();

            alloc_lat_m.collect(alloc_ns);

            timer.pin();
            buddy_->put_free_pages(addr, order);
            auto free_ns = timer.pin();
            free_lat_m.collect(free_ns);
        }

        LOG(INFO) << "buddy_latency: get_free_pages(" << order << ") "
                  << PRE(alloc_lat_m);
        // LOG(INFO) << "buddy_latency: put_free_pages(" << order << ") "
        //           << PRE(free_lat_m);
        df.reg_latency(fmt::format("{}-get_free_pages", order), alloc_lat_m);
    }

    ~Handle()
    {
        dsm_->free(meta_, meta_size_);
        dsm_->free(data_, data_size_);
    }

protected:
    DSM::pointer dsm_;

    std::shared_ptr<avis::BuddyAllocator> buddy_;
    std::shared_ptr<avis::BitmapSlab> bitmap_;
    std::shared_ptr<avis::SlabCache> cache_;

    size_t meta_size_;
    GlobalAddress meta_;
    size_t data_size_;
    GlobalAddress data_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig dsm_config;
    dsm_config.worker_nr = 0;
    auto dsm = DSM::getInstance(dsm_config);
    dsm->registerThread();

    Handle h(dsm);
    h.bench();

    LOG(INFO) << "PASS.";
}