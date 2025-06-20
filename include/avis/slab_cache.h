#pragma once
#include <cstdint>
#include <optional>

#include "./bitmap.h"
#include "./debug.h"
#include "./mm_api.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "Metrics.h"
#include "avis/bitmap.h"
#include "avis/debug.h"
#include "util/Coro.h"
#include "util/Util.h"
#include "util/thread_id.h"

namespace avis
{
/**
 * SlabCache is a per-thread allocation cache backed by (thread-safe) slab
 * allocators.
 * Its purpose is to provide a *steady* number of objects so that
 * the IO to the underlying slab (e.g., batch allocation and deallocation)
 * is reduced.
 *
 * @param conf the config to the cache. In which the water mark, i.e.,
 * `lower_limit` and `upper_limit` are defined.
 */
class SlabCache : public Allocator
{
public:
    constexpr static bool kReport = false;
    constexpr static bool kEnableHistory = false;
    struct Dump
    {
        size_t cache_nr{};
        size_t cache_bytes{};
        Dump &operator+=(const Dump &rhs)
        {
            cache_nr += rhs.cache_nr;
            cache_bytes += rhs.cache_bytes;
            return *this;
        }
        bool empty() const
        {
            return cache_nr == 0;
        }
    };
    SlabCache(std::shared_ptr<BitmapSlab> slab,
              DSM::pointer dsm,
              std::shared_ptr<PTL> ptl,
              CoroContext *ctx)
        : slab_(slab), dsm_(dsm), ptl_(ptl), ctx_(ctx)
    {
    }
    void reset()
    {
        slab_->reset();
    }
    void drain() override
    {
        do_drain(0);
    }
    Dump dump() const
    {
        auto cache_nr = raddrs_.size();
        Dump ret{.cache_nr = cache_nr,
                 .cache_bytes = cache_nr * slab_->object_size()};
        return ret;
    }
    std::pair<AllocMetrics, RemoteMetrics> metrics() const override
    {
        return slab_->metrics();
    }
    bool contains(GlobalAddress raddr) override
    {
        return slab_->contains(raddr);
    }
    bool fast_path_available() const override
    {
        return !raddrs_.empty();
    }
    void report() const override
    {
        slab_->report();
    }
    void metric_reset() override
    {
        slab_->metric_reset();
    }
    GlobalAddress alloc(size_t size) override
    {
        DCHECK_EQ(size, slab_->object_size());
        auto ret = alloc();
        if constexpr (kEnableHistory)
        {
            if (!ret.is_null())
            {
                cache_history_.current().add(HisRecord{
                    .is_alloc = true,
                    .raddr = ret,
                    .size = slab_->object_size(),
                    .tid = (int) util::get_thread_id(),
                    .cid = ctx_ ? ctx_->coro_id() : kNotACoro,
                });
            }
        }
        return ret;
    }
    GlobalAddress alloc()
    {
        // fast path
        if (likely(!raddrs_.empty()))
        {
            auto ret = raddrs_.top();
            raddrs_.pop();
            return ret;
        }

        // Known empty slab.
        if (slab_->empty())
        {
            LOG_IF(WARNING, kReport) << "[SlabCache] slab empty.";
            auto allocated = slab_->get_allocated();
            CHECK_EQ(allocated.size(), slab_->object_nr());
            return GlobalAddress::Null();
        }

        if (unlikely(raddrs_.empty()))
        {
            auto nr = refill();
            LOG_IF(INFO, kReport) << "[SlabCache] refilled " << nr;
        }
        // Failed after refill. Try another.
        if (unlikely(raddrs_.empty()))
        {
            LOG_IF(WARNING, kReport)
                << "[SlabCache] Run out of memory after refill.";
            return GlobalAddress::Null();
        }
        auto ret = raddrs_.top();
        raddrs_.pop();
        return ret;
    }
    GlobalAddress meta_raddr() const override
    {
        return slab_->meta_raddr();
    }

    void free(GlobalAddress raddr) override
    {
        if constexpr (kEnableHistory)
        {
            if (!raddr.is_null())
            {
                cache_history_.current().add(HisRecord{
                    .is_alloc = false,
                    .raddr = raddr,
                    .size = slab_->object_size(),
                    .tid = (int) util::get_thread_id(),
                    .cid = ctx_ ? ctx_->coro_id() : kNotACoro,
                });
            }
        }
        do_free(raddr);
    }

    ~SlabCache()
    {
        // NOTE: don't do_drain here
        // when entering this dctor
        // some resources, e.g., CoroContext may have been dctored.
        // do_drain(0 /* until 0 */);
    }
    BitmapSlab *slab()
    {
        return slab_.get();
    }

private:
    std::shared_ptr<avis::BitmapSlab> slab_;

    DSM::pointer dsm_;
    std::shared_ptr<PTL> ptl_;
    // SlabCacheConfig conf_;
    [[maybe_unused]] CoroContext *ctx_;

    // internal bookkeeping
    std::stack<GlobalAddress> raddrs_;

    size_t do_refill()
    {
        size_t begin_block_nr = Config::ins().cache_fetch_block_nr();
        for (size_t block_nr = begin_block_nr; block_nr < 2 * slab_->block_nr();
             block_nr *= 2)
        {
            size_t allocated = 0;
            slab_->alloc(
                [this, &allocated](GlobalAddress raddr) {
                    raddrs_.push(raddr);
                    allocated++;
                },
                block_nr,
                false /* read_meta */);
            // Okay, we got something.
            // No need to do more work.
            LOG_IF(INFO, kReport) << "[SlabCache] refill block_nr: " << block_nr
                                  << " got object_nr: " << allocated;
            if (allocated)
            {
                return allocated;
            }
        }
        // CHECK(slab_->empty()) << "** Internal Error: You are not empty but "
        //                          "you do not give me anything.";
        return 0;
    }
    size_t object_size() const
    {
        return slab_->object_size();
    }

    size_t refill()
    {
        auto ret = do_refill();
        auto obj_size = object_size();
        if (unlikely(Config::ins().strict_cache_size(obj_size)))
        {
            auto upper_mark = Config::ins().cache_upper_mark(obj_size);
            if (raddrs_.size() >= upper_mark)
            {
                auto expect_size = Config::ins().cache_expect_size(obj_size);
                // LOG(INFO) << "DEBUG: "
                //           << PRE(obj_size, upper_mark, expect_size);
                do_drain(expect_size);
            }
        }
        return ret;
    }

    void do_free(GlobalAddress raddr)
    {
        if constexpr (debug())
        {
            CHECK(slab_->contains(raddr)) << "** " << raddr;
        }
        // if (unlikely(raddrs_.size() > conf_.cache_upper_limit))
        // {
        //     size_t expect_size =
        //         (conf_.cache_lower_limit + conf_.cache_upper_limit) / 2;
        //     do_drain(expect_size);
        // }
        raddrs_.push(raddr);

        auto obj_size = object_size();
        if (unlikely(raddrs_.size() >=
                     Config::ins().cache_upper_mark(obj_size)))
        {
            do_drain(Config::ins().cache_expect_size(obj_size));
        }
    }

    void do_drain(size_t expect_size)
    {
        DCHECK_GE(raddrs_.size(), expect_size);

        auto generator = [&raddr = raddrs_,
                          expect_size]() -> std::optional<GlobalAddress> {
            std::optional<GlobalAddress> ret;
            if (raddr.size() <= expect_size)
            {
                return ret;
            }
            DCHECK(!raddr.empty());
            ret.emplace(raddr.top());
            raddr.pop();
            return ret;
        };

        LOG_IF(INFO, kReport)
            << "[SlabCache] drain object_nr: " << raddrs_.size() - expect_size
            << " remaining " << expect_size;

        slab_->free(generator);

        CHECK_LE(raddrs_.size(), expect_size) << "** drain failed";
    }
};

inline std::ostream &operator<<(std::ostream &os, const SlabCache::Dump &dump)
{
    os << "{in cache obj (nr):" << dump.cache_nr
       << ", total bytes: " << util::pre_byte(dump.cache_bytes) << "}";
    return os;
}

}  // namespace avis