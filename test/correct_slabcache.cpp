#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <unordered_set>

#include "DSM.h"
#include "GlobalAddress.h"
#include "avis/bitmap.h"
#include "avis/slab_cache.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Util.h"
#include "util/bits.h"
#include "util/gflags_def.h"
class NullContainer
{
public:
    template <typename T>
    void insert(T &)
    {
    }
    template <typename T>
    void emplace_back(T &)
    {
    }
    template <typename T>
    void push_back(T &)
    {
    }
};

class Test
{
public:
    constexpr static bool kReport = false;
    Test(DSM::pointer dsm) : dsm_(dsm)
    {
        uint32_t nid = dsm_->getClusterSize() - 1;

        auto bm_data_addr = dsm_->alloc_from((1ull << 4) * 4_KB, nid, 4_KB);
        auto bm_data_size = (1ull << 4) * 4_KB;
        auto ptl = std::make_shared<avis::PTL>(dsm, nid, nullptr);
        auto bitmap = std::make_shared<avis::BitmapSlab>(
            dsm_, bm_data_addr, bm_data_size, 64, ptl, nullptr);
        cache_ = std::make_shared<avis::SlabCache>(bitmap, dsm_, ptl, nullptr);
    }
    ~Test()
    {
    }

    void run()
    {
        hello();
        reset();

        full();
        reset();

        auto *bitmap = CHECK_NOTNULL(cache_->slab());

        auto obj_nr = bitmap->object_nr();
        random(10 * obj_nr, 0, obj_nr);
        reset();

        random(20 * obj_nr, obj_nr * 0.2, obj_nr * 0.8);
        reset();
    }
    void reset()
    {
        cache_->reset();
    }

    void hello()
    {
        auto addr = cache_->alloc();
        CHECK(!addr.is_null());
        cache_->free(addr);
        LOG(INFO) << "PASS hello";
    }

    void full()
    {
        std::unordered_set<GlobalAddress> raddrs;
        while (true)
        {
            auto raddr = cache_->alloc();
            if (raddr.is_null())
            {
                break;
            }
            CHECK(raddrs.insert(raddr).second);
        }
        auto *slab = CHECK_NOTNULL(cache_->slab());
        CHECK_EQ(raddrs.size(), slab->object_nr())
            << "** Leak object: not allocate till full. ";
        for (auto &raddr : raddrs)
        {
            cache_->free(raddr);
        }
        LOG(INFO) << "PASS full";
    }

    void random(size_t test_nr, size_t lower, size_t upper)
    {
        bool alloc_phase = true;
        size_t op = 0;
        while (op < test_nr)
        {
            if (allocated_.size() >= upper)
            {
                alloc_phase = false;
            }
            if (allocated_.size() <= lower || allocated_.empty())
            {
                alloc_phase = true;
            }

            if (alloc_phase)
            {
                auto nr = upper - allocated_.size();
                LOG_IF(INFO, kReport)
                    << "ALLOC " << PRE(nr, upper, allocated_.size());
                CHECK_NE(nr, 0);
                alloc(nr, false /* can fail */);
                op += nr;
            }
            else
            {
                auto nr = allocated_.size() - lower;
                LOG_IF(INFO, kReport)
                    << "FREE " << PRE(nr, lower, allocated_.size());
                CHECK_NE(nr, 0);
                dealloc(nr);
                op += nr;
            }
        }
        LOG(INFO) << "FREE ALL";
        dealloc(allocated_.size());
        cache_->drain();
        LOG(INFO) << fmt::format(
            "PASS random({}, {}, {})", test_nr, lower, upper);
    }

    void alloc(size_t n, bool allow_failed)
    {
        for (size_t i = 0; i < n; ++i)
        {
            auto raddr = cache_->alloc();
            if (likely(!raddr.is_null()))
            {
                LOG_IF(INFO, kReport) << "ALLOC " << PRE(raddr);
                CHECK(allocated_.insert(raddr).second) << PRE(raddr);
            }
            else
            {
                CHECK(allow_failed) << "Unexpected run out of memory";
                break;
            }
        }
        check();
    }
    void dealloc(size_t nr)
    {
        for (size_t i = 0; i < nr; ++i)
        {
            if (!allocated_.empty())
            {
                auto addr = *allocated_.begin();
                CHECK(!addr.is_null());
                LOG_IF(INFO, kReport) << "FREE " << PRE(addr);
                allocated_.erase(addr);
                cache_->free(addr);
            }
            else
            {
                break;
            }
        }
        check();
    }

    void check()
    {
    }

private:
    DSM::pointer dsm_;
    std::shared_ptr<avis::SlabCache> cache_;

    Buffer rdma_buf_;

    std::set<GlobalAddress> allocated_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig config;
    config.worker_nr = 0;

    auto dsm = DSM::getInstance(config);
    dsm->registerThread();

    Test test(dsm);
    test.run();

    LOG(INFO) << "PASS";
    return 0;
}