#pragma once
#include <city.h>

#include <cinttypes>

#include "./config.h"
#include "./locator.h"
#include "./mm_api.h"
#include "./partition_handle.h"
#include "./slab_cache.h"
#include "CoroContext.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "GlobalAllocator.h"
#include "Metrics.h"
#include "avis/bitmap.h"
#include "avis/buddy.h"
#include "avis/provider.h"
#include "avis/publisher.h"
#include "util/CRTP.h"
#include "util/IRdmaAdaptor.h"
#include "util/Likely.h"
#include "util/ThreadSafeHashCache.h"
#include "util/Tracer.h"
#include "util/Util.h"

namespace avis
{
struct BitmapDesc
{
    GlobalAddress buf_raddr;
    size_t size;
    size_t object_size;
};

struct BitmapPub
{
    struct
    {
        GlobalAddress addr;
        uint32_t object_size;
        uint32_t total_size;
    } desc;
    uint64_t crc;

    bool valid() const
    {
        uint64_t cal_crc = CityHash64((char *) &desc, sizeof(desc));
        return cal_crc == crc;
    }
    void setup_crc()
    {
        crc = CityHash64((char *) &desc, sizeof(desc));
    }
} __attribute__((packed));

/**
 * AvisHandle is the main entry of memory management, which offers
 * - high utilization, by slabs over buddy architectures
 * - efficiency, by maintaining cache
 * - easy-to-use, by internally managing multiple allocators (for various size
 * classes, on different nodes, etc)
 */
class AvisHandle : public util::MakeShared<AvisHandle>
{
public:
    constexpr static bool kReport = false;
    constexpr static bool kReportOOM = false;
    using Provider = avis::BuddyProvider;

    AvisHandle(const std::vector<BuddyProvider> &providers,
               DSM::pointer dsm,
               std::shared_ptr<PTL> ptl,
               std::shared_ptr<Publisher<BitmapPub>> pub,
               CoroContext *ctx)
        : buddy_providers_(providers),
          dsm_(dsm),
          ptl_(ptl),
          pub_(pub),
          ctx_(ctx),
          locator_(providers)
    {
        buddy_has_opened_.resize(providers.size(), false);

        if (Config::ins().use_partition_allocator())
        {
            auto node_id = dsm->getClusterSize() - 1;
            partition_handle_ = std::make_unique<avis::PartitionHandle>(
                dsm_, node_id, Config::ins().partition_size());
        }
    }
    void set_ctx(CoroContext *ctx)
    {
        ctx_ = ctx;
        if (ptl_)
        {
            ptl_->set_ctx(ctx);
        }
        if (pub_)
        {
            pub_->set_ctx(ctx);
        }
    }

    GlobalAddress alloc(size_t size)
    {
        if (unlikely(Config::ins().use_partition_allocator()))
        {
            return partition_handle_->alloc(size);
        }
        // too large, use buddy
        if (Config::ins().use_buddy(size))
        {
            auto ret = buddy_alloc(size);
            LOG_IF(INFO, kReportOOM && ret.is_null())
                << "[buddy] buddy_alloc(" << size << ") nullptr";
            return ret;
        }
        else
        {
            size = avis::Config::ins().to_class_size(size);
            auto ret = slab_alloc(size);
            LOG_IF(INFO, kReportOOM && ret.is_null())
                << "[slab] slab_alloc(" << size << ") nullptr";
            return ret;
        }
    }
    auto ptl() const
    {
        return ptl_;
    }

    auto metrics() const
    {
        std::pair<AllocMetrics, RemoteMetrics> ret;
        // don't count in buddy: duplicated with slabs
        // for (const auto &buddy : buddys_)
        // {
        //     auto p = buddy->metrics();
        //     ret.first += p.first;
        //     ret.second += p.second;
        // }
        for (const auto &[size, ctx] : alloc_ctxs_)
        {
            for (const auto &alloc : ctx.allocators_)
            {
                auto p = alloc->metrics();
                ret.first += p.first;
                ret.second += p.second;
                // LOG(INFO) << "tmp: " << p.first << ", " << p.second;
            }
        }
        return ret;
    }
    PartitionMetric partition_metric() const
    {
        if (Config::ins().use_partition_allocator())
        {
            return partition_handle_->partition_metric();
        }
        return {};
    }

    void free(GlobalAddress raddr, size_t size)
    {
        if (unlikely(Config::ins().drop_deallocated_obj()))
        {
            // DROP THIS OBJECT.
            return;
        }
        if (unlikely(Config::ins().use_partition_allocator()))
        {
            return partition_handle_->free(raddr, size);
        }
        if (Config::ins().use_buddy(size))
        {
            buddy_free(raddr, size);
        }
        else
        {
            size = avis::Config::ins().to_class_size(size);
            slab_free(raddr, size);
        }
    }

    auto dsm() const
    {
        return dsm_;
    }
    CoroContext *coro_ctx() const
    {
        return ctx_;
    }

    void prepare_read(char *buffer, GlobalAddress gaddr, size_t size)
    {
        dsm_->prepare_read(buffer, gaddr, size, false /* on chip */, ctx_);
    }

    void prepare_write(char *buffer, GlobalAddress gaddr, size_t size)
    {
        dsm_->prepare_write(buffer, gaddr, size, false /* on chip */, ctx_);
    }
    void prepare_cas(GlobalAddress gaddr,
                     size_t size,
                     uint64_t compare,
                     uint64_t compare_mask,
                     uint64_t swap,
                     uint64_t swap_mask,
                     void *rdma_buffer)
    {
        dsm_->prepare_cas(gaddr,
                          size,
                          compare,
                          compare_mask,
                          swap,
                          swap_mask,
                          rdma_buffer,
                          false /* on chip */,
                          ctx_);
    }

    void prepare_faa(GlobalAddress gaddr,
                     size_t size,
                     uint64_t add_val,
                     uint64_t field_boundary,
                     void *rdma_buffer)
    {
        dsm_->prepare_faa(gaddr,
                          size,
                          add_val,
                          field_boundary,
                          rdma_buffer,
                          false /* on chip */,
                          ctx_);
    }
    void commit(util::TraceView trace = util::nulltrace)
    {
        dsm_->commit(ctx_, trace);
    }

    void drain()
    {
        for (const auto &[size, ctx] : alloc_ctxs_)
        {
            for (const auto &alloc : ctx.allocators_)
            {
                alloc->drain();
            }
        }
    }

    void report() const
    {
        for (const auto &[_, buddy] : buddys_)
        {
            buddy->report();
        }
        for (const auto &[size, ctx] : alloc_ctxs_)
        {
            for (const auto &alloc : ctx.allocators_)
            {
                alloc->report();
            }
        }
    }
    void metric_reset()
    {
        for (const auto &[_, buddy] : buddys_)
        {
            buddy->metric_reset();
        }
        for (const auto &[size, ctx] : alloc_ctxs_)
        {
            for (const auto &alloc : ctx.allocators_)
            {
                alloc->metric_reset();
            }
        }
    }

    struct Dump
    {
        size_t buddy_nr;
        std::map<size_t, std::vector<avis::SlabCache::Dump>> cache_dump;
    };
    Dump dump()
    {
        Dump ret;
        ret.buddy_nr = buddys_.size();
        for (auto &[size, ctx] : alloc_ctxs_)
        {
            for (auto &alloc : ctx.allocators_)
            {
                auto *bitmap = dynamic_cast<avis::SlabCache *>(alloc.get());
                ret.cache_dump[size].emplace_back(bitmap->dump());
            }
        }
        return ret;
    }

    ~AvisHandle()
    {
        // LOG(WARNING) << "avis::~AvisHandle()";
    }
    const auto &get_buddys() const
    {
        return buddys_;
    }

private:
    const std::vector<BuddyProvider> buddy_providers_;
    std::vector<bool> buddy_has_opened_;

    // don't move buddy_idx unless allocation fail
    // we need locality for utilization
    size_t buddy_idx_{0};
    DSM::pointer dsm_;
    std::shared_ptr<PTL> ptl_;
    std::shared_ptr<Publisher<BitmapPub>> pub_;
    CoroContext *ctx_;

    Locator locator_;

    std::unique_ptr<avis::PartitionHandle> partition_handle_;

    // for slabs
    struct AllocCtx
    {
        std::vector<std::shared_ptr<Allocator>> allocators_;
        size_t idx_{};  // for RR
    };
    std::map<size_t, AllocCtx> alloc_ctxs_;

    // buddy_id to buddy
    std::unordered_map<size_t, std::shared_ptr<BuddyAllocator>> buddys_;

    GlobalAddress slab_alloc(size_t size)
    {
        auto ret = do_slab_alloc(size);

        if (likely(!ret.is_null()))
        {
            return ret;
        }

        // try opening existing slabs
        if (Config::ins().share_bitmap(size))
        {
            if (pub_ && pub_->check_update())
            {
                for (size_t i = 0; i < pub_->size(); ++i)
                {
                    const auto &bitmap_desc = *DCHECK_NOTNULL(pub_->object(i));
                    if (!bitmap_desc.valid())
                    {
                        continue;
                    }
                    auto *slab = open_existing_slab_directly_at(
                        bitmap_desc.desc.addr,
                        bitmap_desc.desc.total_size,
                        bitmap_desc.desc.object_size);
                    // NOTE: only allocate when slab class matches
                    if (slab && bitmap_desc.desc.object_size == size)
                    {
                        auto ret_again = slab->alloc(size);
                        if (!ret_again.is_null())
                        {
                            return ret_again;
                        }
                    }
                }
            }
        }

        LOG_IF(INFO, kReportOOM) << "[slab] slab runs out of memory. Try "
                                    "create new slab of object size "
                                 << size;
        // slab run out of memory
        // create new slab
        bool ok = create_slab(size);
        if (unlikely(!ok))
        {
            LOG_IF(INFO, kReportOOM)
                << "[slab] Failed to create_slab(" << size << ")";
            return GlobalAddress::Null();
        }
        auto again = do_slab_alloc(size);
        CHECK(!again.is_null()) << "** Failed to do_slab_alloc(" << size
                                << ") from a just-created slab.";
        return again;

        // return do_slab_alloc(size);
    }

    GlobalAddress do_slab_alloc(size_t size)
    {
        auto &ctx = alloc_ctxs_[size];

        // First, try to allocate from fast path
        for (size_t i = 0; i < ctx.allocators_.size(); ++i)
        {
            auto id = (ctx.idx_ + i) % ctx.allocators_.size();
            auto &alloc = ctx.allocators_[id];
            if (alloc->fast_path_available())
            {
                auto ret = alloc->alloc(size);
                CHECK(!ret.is_null())
                    << "** If fast path is available, it can not fail";
                ctx.idx_ = id;
                return ret;
            }
        }

        // Failed. Go to the slow path
        for (size_t i = 0; i < ctx.allocators_.size(); ++i)
        {
            auto id = (ctx.idx_ + i) % ctx.allocators_.size();
            auto &alloc = ctx.allocators_[id];
            auto ret = alloc->alloc(size);
            if (!ret.is_null())
            {
                LOG_IF(INFO, kReport) << "[avis] batch_alloc " << size
                                      << " objects from slab(" << i << ")";
                ctx.idx_ = id;
                return ret;
            }
        }
        return GlobalAddress::Null();
    }

    void slab_free(GlobalAddress raddr, size_t size)
    {
        auto &slab = locate_slab(raddr, size);
        slab.free(raddr);
    }

    // This must succeed
    Allocator &locate_slab(GlobalAddress addr, size_t size)
    {
        auto it = alloc_ctxs_.find(size);
        // no such size class. open existing slab
        if (unlikely(it == alloc_ctxs_.end()))
        {
            auto *slab = open_existing_slab_containing(addr, size);
            LOG_IF(FATAL, slab == nullptr)
                << "** failed to open_existing_slab: " << PRE(addr, size);
            DCHECK(slab->contains(addr));
            return *slab;
        }
        auto &ctx = it->second;
        for (auto &alloc : ctx.allocators_)
        {
            if (alloc->contains(addr))
            {
                DCHECK(alloc->contains(addr));
                return *alloc;
            }
        }
        // no such slab. open existing slab
        auto *slab = open_existing_slab_containing(addr, size);
        LOG_IF(FATAL, slab == nullptr)
            << "** failed to open_existing_slab: " << PRE(addr, size);
        DCHECK(slab->contains(addr)) << addr;
        return *slab;
    }

    Allocator *open_existing_slab_directly_at(GlobalAddress addr,
                                              size_t total_size,
                                              size_t object_size)
    {
        // first, check conflict
        auto &ctx = alloc_ctxs_[object_size];
        for (const auto &alloc : ctx.allocators_)
        {
            // already open
            if (alloc->meta_raddr() == addr)
            {
                return nullptr;
            }
        }

        auto slab = std::make_shared<BitmapSlab>(
            dsm_, addr, total_size, object_size, ptl_, ctx_);
        // NOTE: don't call slab->init()
        // it overwrites existing meta
        auto cache = std::make_shared<SlabCache>(slab, dsm_, ptl_, ctx_);

        ctx.allocators_.emplace_back(cache);
        ctx.idx_ = ctx.allocators_.size() - 1;

        return cache.get();
    }

    Allocator *open_existing_slab_containing(GlobalAddress addr,
                                             size_t object_size)
    {
        auto locate = locator_.locate(addr, object_size);

        LOG_IF(INFO, kReport)
            << "[avis] try to open_existing_slab for object size "
            << object_size << ". locate at " << util::pre(locate);

        if (unlikely(!locate))
        {
            LOG(ERROR) << "Failed to locate " << PRE(addr, object_size);
            return nullptr;
        }
        LOG_IF(INFO, kReport)
            << "[avis] try to open_existing_slab for object size "
            << object_size;
        auto slab = std::make_shared<BitmapSlab>(dsm_,
                                                 locate->bitmap_meta,
                                                 locate->bitmap_size,
                                                 object_size,
                                                 ptl_,
                                                 ctx_);
        // NOTE: don't call slab->init()
        // it overwrites existing meta
        auto cache = std::make_shared<SlabCache>(slab, dsm_, ptl_, ctx_);

        auto &ctx = alloc_ctxs_[object_size];
        ctx.allocators_.emplace_back(cache);
        // NOTE: don't update ctx.idx_

        return cache.get();
    }

    bool create_slab(size_t size)
    {
        LOG_IF(INFO, kReport)
            << "[avis] try to create_slab for object size " << size;

        auto bitmap_size = Config::ins().bitmap_size(size);
        auto addr = buddy_alloc(bitmap_size);
        if (addr.is_null())
        {
            LOG_IF(INFO, kReportOOM)
                << "[slab] failed to create_slab: buddy_alloc(" << bitmap_size
                << ") get nullptr. From object size " << size;
            return false;
        }

        auto slab = std::make_shared<BitmapSlab>(
            dsm_, addr, bitmap_size, size, ptl_, ctx_);
        slab->init();
        // TODO: we may need to populate the slab to the global memory
        // so that it is shared
        auto cache = std::make_shared<SlabCache>(slab, dsm_, ptl_, ctx_);

        auto &ctx = alloc_ctxs_[size];
        ctx.allocators_.emplace_back(cache);
        ctx.idx_ = ctx.allocators_.size() - 1;

        // publish this slab
        if (Config::ins().share_bitmap(size))
        {
            if (pub_)
            {
                BitmapPub content{.desc = {
                                      .addr = addr,
                                      .object_size = (uint32_t) size,
                                      .total_size = (uint32_t) bitmap_size,
                                  }};
                content.setup_crc();
                pub_->publish(content);
            }
        }

        return true;
    }

    GlobalAddress do_buddy_alloc(size_t size)
    {
        // fast path
        if (likely(buddys_.contains(buddy_idx_)))
        {
            auto &cur_buddy = buddys_[buddy_idx_];
            unsigned order =
                ceil(log2(round_up_div(size, cur_buddy->page_size())));
            auto ret = cur_buddy->get_free_pages(order);
            if (likely(!ret.is_null()))
            {
                return ret;
            }
            LOG_IF(INFO, kReportOOM)
                << "[buddy] failed to alloc fast path: " << PRE(size);
        }

        // failed, go to slow path
        for (auto &[buddy_id, buddy] : buddys_)
        {
            // already tried and failed
            if (buddy_id == buddy_idx_)
            {
                continue;
            }
            unsigned order = ceil(log2(round_up_div(size, buddy->page_size())));

            auto ret = buddy->get_free_pages(order);
            if (likely(!ret.is_null()))
            {
                buddy_idx_ = buddy_id;
                return ret;
            }
        }
        LOG_IF(INFO, kReportOOM)
            << "[buddy] failed to alloc slow path: " << PRE(size);
        return GlobalAddress::Null();
    }

    GlobalAddress buddy_alloc(size_t size)
    {
        auto ret = do_buddy_alloc(size);
        if (likely(!ret.is_null()))
        {
            return ret;
        }

        for (size_t i = 0; i < 4; ++i)
        {
            LOG_IF(INFO, kReportOOM) << "[buddy] failed to alloc " << PRE(size)
                                     << ": try to open new buddy";
            bool ok = try_open_buddy();
            if (unlikely(!ok))
            {
                LOG_IF(INFO, kReportOOM)
                    << "[buddy] failed to open new buddy. Run out of memory.";
                return GlobalAddress::Null();
            }
            ret = do_buddy_alloc(size);
            if (likely(!ret.is_null()))
            {
                return ret;
            }
        }
        LOG_IF(INFO, kReportOOM) << "[buddy] failed to open new buddy 4 times";
        return GlobalAddress::Null();
    }

    void buddy_free(GlobalAddress raddr, size_t size)
    {
        for (auto &[_, buddy] : buddys_)
        {
            if (buddy->contains(raddr))
            {
                auto page_size = buddy->page_size();
                DCHECK_EQ(size % page_size, 0);
                unsigned int order = log2(size / page_size);
                buddy->put_free_pages(raddr, order);
                return;
            }
        }
        LOG(FATAL) << "[Avis] The page run is not allocated from me: "
                   << PRE(raddr, size);
    }

    bool try_open_buddy()
    {
        for (size_t i = 0; i < buddy_providers_.size(); ++i)
        {
            if (buddy_has_opened_[i])
            {
                continue;
            }
            // this buddy not opened.
            CHECK(do_open_buddy(i))
                << "** this must okay because we checked has_opened";
            buddy_has_opened_[i] = true;
            LOG_IF(INFO, kReportOOM) << "[buddy] opened buddy(" << i << ")";
            return true;
        }
        return false;
    }

    bool do_open_buddy(size_t buddy_id)
    {
        auto it = buddys_.find(buddy_id);
        // already exists
        if (unlikely(it != buddys_.end()))
        {
            return false;
        }

        auto &provider = buddy_providers_[buddy_id];
        DCHECK(!buddy_has_opened_[buddy_id]);

        auto buddy = std::make_shared<BuddyAllocator>(dsm_,
                                                      provider.meta_raddr,
                                                      provider.meta_size,
                                                      provider.buf_raddr,
                                                      provider.buf_size,
                                                      provider.page_size,
                                                      ptl_,
                                                      ctx_);
        bool ok = buddys_.emplace(buddy_id, buddy).second;
        DCHECK(ok);
        return true;
    }
};
inline std::ostream &operator<<(std::ostream &os, const AvisHandle::Dump &d)
{
    os << "{\nbuddy: " << d.buddy_nr << std::endl;
    avis::SlabCache::Dump total_in_total;
    for (const auto &[size, dumps] : d.cache_dump)
    {
        os << "[" << size << "]: ";
        avis::SlabCache::Dump total;
        size_t empty_cache_nr = 0;
        for (const auto &dump : dumps)
        {
            total += dump;
            // if (!dump.empty())
            // {
            //     os << dump << " ";
            // }
            // else
            // {
            //     empty_cache_nr++;
            // }
            if (dump.empty())
            {
                empty_cache_nr++;
            }
        }
        os << "Total: " << total << " with " << empty_cache_nr
           << " empty caches out of " << dumps.size() << std::endl;
        total_in_total += total;
    }
    os << "} " << std::endl;
    os << "Summary: " << total_in_total;
    return os;
}
}  // namespace avis