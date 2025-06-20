#pragma once
#include "CoroContext.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "util/DSMBackend.h"
#include "util/Page.h"
#include "util/ThreadSafeHashCache.h"
#include "util/lock/TicketLock.h"

namespace sherman
{
// TODO: page cache has lots of opportunities for copy ellision
class PageCache
{
public:
    using Page = util::Page;

    PageCache(DSM::pointer dsm, size_t bucket_nr, size_t cache_limit)
        : dsm_(dsm),
          bucket_nr_(bucket_nr),
          cache_limit_(cache_limit),
          cache_(util::DSMBackend::make_ptr(dsm), bucket_nr_, cache_limit_),
          locks_(bucket_nr)
    {
    }
    bool invalidate_aligned(GlobalAddress page_gaddr, CoroContext *ctx)
    {
        DCHECK(util::is_aligned(page_gaddr.offset, kInternalPageSize));
        DCHECK(!page_gaddr.is_null());
        auto old_val = cache_invalidate(page_gaddr, ctx);
        return old_val != nullptr;
    }
    bool invalidate_unaligned(GlobalAddress gaddr, CoroContext *ctx)
    {
        DCHECK(!gaddr.is_null());
        GlobalAddress aligned_gaddr = gaddr;
        aligned_gaddr.offset =
            util::to_aligned(aligned_gaddr.offset, kInternalPageSize);
        CHECK_NE(gaddr, aligned_gaddr);
        return invalidate_aligned(aligned_gaddr, ctx);
    }
    bool invalidate(GlobalAddress gaddr, CoroContext *ctx)
    {
        if (util::is_aligned(gaddr.offset, kInternalPageSize))
        {
            return invalidate_aligned(gaddr, ctx);
        }
        else
        {
            return invalidate_unaligned(gaddr, ctx);
        }
    }

    // precondition: write lock protected for the page
    void write_aligned(Page &&page,
                       GlobalAddress gaddr,
                       size_t size,
                       CoroContext *ctx)
    {
        // LOG(INFO) << "Write " << gaddr;
        DCHECK(!gaddr.is_null());
        DCHECK(util::is_aligned(gaddr.offset, kInternalPageSize));
        DCHECK_EQ(size, kInternalPageSize);

        cache_put(gaddr, std::move(page), ctx);
    }
    // precondition: write lock protected for the page
    // postcondition: the page is moved
    void write_aligned(Page &&page, GlobalAddress gaddr, CoroContext *ctx)
    {
        // LOG(INFO) << "Write " << gaddr;
        DCHECK(!gaddr.is_null());
        DCHECK(util::is_aligned(gaddr.offset, kInternalPageSize));
        DCHECK_EQ(page.size(), kInternalPageSize);

        cache_put(gaddr, std::move(page), ctx);
    }

    // local buffer: page.data() + page_offset
    // remote gaddr: page_gaddr + page_offset
    void write_unaligned(Page &&page,
                         GlobalAddress page_gaddr,
                         off_t page_offset,
                         size_t size,
                         CoroContext *ctx)
    {
        // LOG(INFO) << "Write partial " << page_gaddr;
        DCHECK(!page_gaddr.is_null());
        DCHECK(util::is_aligned(page_gaddr.offset, kInternalPageSize));

        // extend life of page: shared_ptr
        auto page_ptr = cache_get_or_fetch(page_gaddr, ctx);
        // copy here:
        // we must make a copy then re-insert into the cache
        // to avoid cocurrent eviction causing modification loss:
        // the page is evicted and then modified.
        auto new_page = *page_ptr;  // COPY
        memcpy(new_page.data() + page_offset, page.data() + page_offset, size);
        cache_put(page_gaddr, std::move(new_page), ctx);
    }

    // CONTRACT:
    // - return value is read-only
    // - (gaddr, size) is aligned
    [[nodiscard]] auto read_aligned_no_modify(GlobalAddress gaddr,
                                              size_t size,
                                              CoroContext *ctx)
    {
        // LOG(INFO) << "Read " << gaddr;

        DCHECK(!gaddr.is_null());
        DCHECK(util::is_aligned(gaddr.offset, kInternalPageSize));
        DCHECK_EQ(size, kInternalPageSize);

        return cache_get_or_fetch(gaddr, ctx);
    }

    // precondition: write lock is held
    void write_page_and_unlock(Page &&page,
                               GlobalAddress page_gaddr,
                               off_t page_offset,
                               size_t write_size,
                               [[maybe_unused]] uint64_t *cas_buffer,
                               GlobalAddress lock_addr,
                               [[maybe_unused]] uint64_t tag,
                               CoroContext *ctx,
                               int coro_id,
                               bool async)
    {
        // LOG(INFO) << "Write + unlock " << page_gaddr;

        DCHECK(!page_gaddr.is_null());
        DCHECK(util::is_aligned(page_gaddr.offset, kInternalPageSize));
        if (page_offset == 0)
        {
            write_aligned(std::move(page), page_gaddr, write_size, ctx);
        }
        else
        {
            write_unaligned(
                std::move(page), page_gaddr, page_offset, write_size, ctx);
        }
        unlock_addr(lock_addr, tag, ctx, coro_id, async);
    }
    /**
     * CONTRACT: return value is read-only
     */
    [[nodiscard]] auto lock_and_read_page_no_modify(GlobalAddress page_addr,
                                                    size_t page_size,
                                                    uint64_t *cas_buffer,
                                                    GlobalAddress lock_gaddr,
                                                    uint64_t tag,
                                                    CoroContext *ctx,
                                                    int coro_id)
    {
        // LOG(INFO) << "Lock + read " << page_addr;
        DCHECK(!page_addr.is_null());
        try_lock_addr(lock_gaddr, tag, cas_buffer, ctx, coro_id);
        return read_aligned_no_modify(page_addr, page_size, ctx);
    }
    // always succeed and return true
    void try_lock_addr(GlobalAddress lock_addr,
                       [[maybe_unused]] uint64_t tag,
                       [[maybe_unused]] uint64_t *buf,
                       CoroContext *ctx,
                       int coro_id)
    {
        // LOG(INFO) << "Lock " << lock_addr;
        DCHECK_LT(lock_addr.nodeID, MAX_MACHINE);
        DCHECK_LT(lock_addr.offset / 8, define::kNumOfLock);
        auto lock_idx =
            lock_addr.nodeID * define::kNumOfLock + (lock_addr.offset / 8);

        auto &queue = lock_pending_queue();
        locks_.acquire(lock_idx, ctx, &queue);
        DCHECK(queue.empty() || queue.front() != ctx->coro_id())
            << "** master does not react to the queue: .front() is still me";
        if constexpr (debug())
        {
            if (ctx)
            {
                DCHECK_EQ(ctx->coro_id(), coro_id);
            }
        }
    }
    void unlock_addr(GlobalAddress lock_addr,
                     [[maybe_unused]] uint64_t tag,
                     CoroContext *ctx,
                     int coro_id,
                     [[maybe_unused]] bool async)
    {
        // LOG(INFO) << "Unlock " << lock_addr;
        if constexpr (debug())
        {
            CHECK_LT(lock_addr.nodeID, MAX_MACHINE);
            CHECK_LT(lock_addr.offset / 8, define::kNumOfLock);
            if (ctx)
            {
                CHECK_EQ(coro_id, ctx->coro_id());
            }
        }

        auto lock_idx =
            lock_addr.nodeID * define::kNumOfLock + (lock_addr.offset / 8);
        locks_.release(lock_idx, ctx);
    }
    std::queue<int> &lock_pending_queue()
    {
        return lock_pending_queue_.current();
    }
    constexpr size_t cache_limit() const
    {
        return cache_limit_;
    }
    constexpr size_t bucket_nr() const
    {
        return bucket_nr_;
    }
    auto &cache()
    {
        return cache_;
    }

private:
    DSM::pointer dsm_;
    size_t bucket_nr_;
    size_t cache_limit_;

    // GlobalAddress => page
    using K = GlobalAddress;  // global address
    using V = Page;
    using entry_t = std::pair<K, V>;
    util::hash::ThreadSafeHashCache<K, V> cache_;

    util::TicketLockManager locks_;
    Perthread<std::queue<int>> lock_pending_queue_;

    // pre-condition: size == kInternalPageSize
    std::shared_ptr<V> cache_get(GlobalAddress gaddr, CoroContext *ctx)
    {
        return cache_.get(gaddr, false /* read backend */, ctx);
    }
    std::shared_ptr<V> cache_get_or_fetch(GlobalAddress gaddr, CoroContext *ctx)
    {
        return DCHECK_NOTNULL(cache_.get(gaddr, true /* read backend */, ctx));
    }

    void cache_put(GlobalAddress gaddr, V &&new_page, CoroContext *ctx)
    {
        DCHECK(new_page.is_DMA());
        cache_.put(gaddr, std::move(new_page), ctx);
    }
    std::shared_ptr<V> cache_invalidate(GlobalAddress gaddr, CoroContext *ctx)
    {
        return cache_.invalidate(gaddr, ctx);
    }
};
}  // namespace sherman