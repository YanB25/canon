#pragma once
#include <cinttypes>

#include "./api.h"
#include "./debug.h"
#include "DSM.h"
#include "DSMCache.h"
#include "Metrics.h"
#include "avis/layout.h"
#include "avis/mm.h"
#include "avis/policy.h"
#include "memory/allocator.h"
#include "util/CRTP.h"
#include "util/Hexdump.hpp"
#include "util/Tracer.h"
#include "util/bits.h"
#include "util/thread_id.h"

namespace avis::policy::batch
{
struct AllocCtx
{
    std::optional<uint32_t> select_block_idx;
    std::optional<uint32_t> select_slot_idx;
    std::optional<uint32_t> select_page_idx;  // only for base frame
    // this is per-block:
    // when switching block, clear me or (re)-read me.
    std::optional<Buffer> cached_central_bitmap;
    size_t polled_block = 0;
    volatile char *locate_bitmap(const Layout &lay, size_t slot_id)
    {
        volatile char *p_bitmap =
            cached_central_bitmap->buffer + lay.block_central_size();
        auto bitmap_sz = lay.block_bitmap_entry_size();
        volatile char *p_cur_bitmap = p_bitmap + slot_id * bitmap_sz;

        cached_central_bitmap->assert_contains((void *) p_bitmap, bitmap_sz);
        return p_cur_bitmap;
    }
};

struct FreeCtx
{
    std::optional<Buffer> cached_central_bitmap;
    std::optional<size_t> block_id;
    volatile char *locate_bitmap(const Layout &lay, size_t slot_id)
    {
        volatile char *p_bitmap =
            cached_central_bitmap->buffer + lay.block_central_size();
        auto bitmap_sz = lay.block_bitmap_entry_size();
        volatile char *p_cur_bitmap = p_bitmap + slot_id * bitmap_sz;
        cached_central_bitmap->assert_contains((void *) p_cur_bitmap,
                                               bitmap_sz);
        return p_cur_bitmap;
    }
};

inline std::ostream &operator<<(std::ostream &os, const AllocCtx &a)
{
    os << "block " << *a.select_block_idx << " slot " << *a.select_slot_idx
       << " page " << *a.select_page_idx;
    return os;
}

class AvisPoolAllocator : public AvisAllocator
{
public:
    constexpr static bool kReportAlloc = false;
    constexpr static bool kReportSelect = false;
    constexpr static bool kReportRDMA = false;
    constexpr static bool kEnableHistory = false;

    AvisPoolAllocator(DSM::pointer dsm,
                      uint16_t node_id,
                      uint16_t region_id,
                      uint32_t rkey,
                      [[maybe_unused]] void *pool_meta_addr,
                      void *buffer_addr,
                      const Config &conf,
                      CoroContext *ctx,
                      const Policy &policy)
        : dsm_(dsm),
          node_id_(node_id),
          region_id_(region_id),
          rkey_(rkey),
          conf_(conf),
          ctx_(ctx),
          policy_(policy),
          pool_view_(pool_meta_addr, buffer_addr, conf_)
    {
        auto block_meta_size = layout().block_meta_size();
        free_ctx.cached_central_bitmap = dsm_->get_rdma_buffer(block_meta_size);
    }
    void validate_addr(uint64_t addr, size_t size)
    {
        Buffer access((char *) addr, size);
        auto *meta_addr = pool_view_.meta_addr();
        auto meta_len = layout().total_meta_size();
        Buffer meta((char *) meta_addr, meta_len);
        validate_buffer_not_overlapped(access, meta);
        // LOG(INFO) << PRE((void *) addr, (void *) meta_addr);
    }
    AvisAddress alloc(size_t size) override
    {
        if (size <= conf_.base_frame_size)
        {
            if (unlikely(cached_avis_.empty()))
            {
                bool succ = refill();
                if (unlikely(!succ))
                {
                    return avis_address_from_offset(0);
                }
            }
            DCHECK(!cached_avis_.empty()) << "** refill failed.";
            auto ret = cached_avis_.top();
            cached_avis_.pop();
            return ret;
        }
        else
        {
            auto ret = do_alloc_huge();
            return ret;
        }
    }
    bool refill()
    {
        while (true)
        {
            auto state = select_next_base();
            if (state == State::kFull)
            {
                LOG(WARNING)
                    << "** Failed to refill: no base page: " << PRE(state);
                return false;
            }
            auto ret = try_allocate_huge_breakable();
            if (ret)
            {
                return true;
            }
        }
    }
    uint64_t try_allocate_huge_breakable()
    {
        auto &c = alloc_ctx;
        auto block_id = *c.select_block_idx;
        auto slot_id = *c.select_slot_idx;

        // LOG_IF(INFO, kReportAlloc)
        //     << "Try allocate huge breakable at " << PRE(c);

        auto lay = layout();

        auto page_nr_per_slot = lay.entry_nr_per_bitmap();

        uint64_t all_bitmap_raddr =
            (uint64_t) pool_view_.ith_block_bitmap_meta_addr(block_id);
        auto bitmap_sz = lay.block_bitmap_entry_size();
        uint64_t cur_bitmap_raddr = all_bitmap_raddr + slot_id * bitmap_sz;

        std::vector<char> mask(bitmap_sz, 0xff);
        std::vector<char> before_cmp_val(bitmap_sz, 0);

        char *p_bitmap = (char *) c.locate_bitmap(lay, slot_id);
        c.cached_central_bitmap->assert_contains(p_bitmap, bitmap_sz);
        memset(p_bitmap, 0, bitmap_sz);

        // NOTE: Retry more than two times do not make sense
        // If CAS failed
        // => concurrent clients
        // => The client will allocate all pages in this huge page.
        uint64_t get_nr = 0;
        for (size_t retry = 0; retry < 2; ++retry)
        {
            c.cached_central_bitmap->assert_contains(p_bitmap, bitmap_sz);
            memcpy(before_cmp_val.data(), p_bitmap, bitmap_sz);
            dsm_->prepare_cas(node_id_,
                              rkey_,
                              cur_bitmap_raddr,
                              bitmap_sz,
                              (uint64_t) p_bitmap /* compare val */,
                              (uint64_t) mask.data() /* compare mask*/,
                              (uint64_t) mask.data() /* swap val */,
                              (uint64_t) mask.data() /* swap mask*/,
                              (void *) p_bitmap,
                              ctx_);
            dsm_->commit(ctx_);

            auto bitmap_bits = util::BitsViewMut(p_bitmap, bitmap_sz);
            // NOTE: according to spec,
            // we need to a bswap64 for extended atomics > 8_B.
            if (likely(bitmap_sz > 8))
            {
                bitmap_bits.bswap64();
            }

            auto zeros_nr = bitmap_bits.count_zeros();

            if (memcmp((void *) p_bitmap, before_cmp_val.data(), bitmap_sz) ==
                0)
            {
                // each set bit in p_bitmap (old value)
                // denotes one available base page
                for (size_t i = 0; i < page_nr_per_slot; ++i)
                {
                    // unset bits (i.e., 0) means available
                    if (bitmap_bits.is_unset(i))
                    {
                        auto avis_addr = avis_address_from_offset(
                            to_offset(block_id, slot_id, i));
                        get_nr++;
                        cached_avis_.push(avis_addr);
                    }
                }

                DCHECK_GT(get_nr, 0) << "bitmap: " << std::endl
                                     << bitmap_bits.hex_dump() << std::endl
                                     << "before_cmp_val: " << std::endl
                                     << util::Hexdump(before_cmp_val.data(),
                                                      before_cmp_val.size());
                // maintain p_bitmap
                const auto &swap = mask;
                memcpy(p_bitmap, swap.data(), bitmap_sz);
                LOG_IF(INFO, kReportAlloc)
                    << "Refill: " << PRE(get_nr) << " at " << PRE(c);
                return get_nr;
            }
            else
            {
                bool available = zeros_nr > 0;
                if (!available)
                {
                    return 0;
                }
                // CAS failed, retry.
            }
        }

        return get_nr;
    }

    // drain return all pages to MN
    void drain()
    {
        struct DrainCtx
        {
            std::optional<Buffer> bitmap;
            std::vector<char> buffer;
            std::unordered_set<uint64_t> modified_unit_idxs;
        };
        // block_id => DrainCtx
        std::unordered_map<uint64_t, DrainCtx> drain_ctxs;

        auto bitmap_sz = layout().block_bitmap_entry_size();
        auto bitmap_nr = layout().bitmap_nr_per_block();
        auto page_nr_per_slot = layout().entry_nr_per_bitmap();

        auto io_unit = 32;  // 32 byte for a unit
        auto bit_nr_per_unit = io_unit * 8;

        while (!cached_avis_.empty())
        {
            auto ret = cached_avis_.top();
            cached_avis_.pop();
            auto [block_id, slot_id, page_id] =
                pool_view_.offset_to_position(ret.offset());

            auto it = drain_ctxs.find(block_id);
            if (unlikely(it == drain_ctxs.end()))
            {
                it = drain_ctxs.emplace(block_id, DrainCtx{}).first;
                it->second.bitmap =
                    dsm_->get_rdma_buffer(bitmap_sz * bitmap_nr);
                memset(it->second.bitmap->buffer, 0, bitmap_sz * bitmap_nr);
                it->second.buffer.resize(bitmap_sz * bitmap_nr, 0);
            }
            // `id` is flatten across the whole (multiple) bitmap space
            auto id = slot_id * page_nr_per_slot + page_id;
            auto unit_id = id / bit_nr_per_unit;
            auto buffer_bits = util::BitsViewMut(it->second.buffer.data(),
                                                 it->second.buffer.size());
            DCHECK_LT(id / 8, it->second.buffer.size())
                << "bit " << PRE(id) << " overflowed";
            DCHECK(buffer_bits.is_unset(id))
                << "** double free detected: already set " << PRE(id)
                << std::endl
                << buffer_bits.hex_dump();
            buffer_bits.set(id);
            it->second.modified_unit_idxs.insert(unit_id);
        }

        // The base is
        size_t batch_limit = 8;
        size_t cur_size = 0;
        std::vector<char> field_boundary(io_unit, 0xff);
        for (auto &[block_id, dctx] : drain_ctxs)
        {
            uint64_t bitmap_addr =
                (uint64_t) pool_view_.ith_block_bitmap_meta_addr(block_id);
            for (auto unit_id : dctx.modified_unit_idxs)
            {
                auto offset = unit_id * io_unit;
                uint64_t remote_addr = bitmap_addr + offset;

                char *padd_val = dctx.buffer.data() + offset;
                DCHECK_GT(io_unit, 8);
                char *prdma_buf = dctx.bitmap->buffer + offset;

                dsm_->prepare_faa(node_id_,
                                  rkey_,
                                  remote_addr,
                                  io_unit,
                                  (uint64_t) padd_val,
                                  (uint64_t) field_boundary.data(),
                                  prdma_buf,
                                  ctx_);

                cur_size++;
                m_.faa(io_unit);
                if (cur_size >= batch_limit)
                {
                    dsm_->commit(ctx_);
                    cur_size = 0;
                }
            }
        }
        if (cur_size)
        {
            dsm_->commit(ctx_);
            cur_size = 0;
        }
        for (auto &&[_, dctx] : drain_ctxs)
        {
            dsm_->put_rdma_buffer(std::move(*dctx.bitmap));
        }
    }

    void free(const AvisAddress &addr, size_t size) override
    {
        if (size > conf_.base_frame_size)
        {
            free_huge(addr);
        }
        else
        {
            cached_avis_.push(addr);
            if (unlikely(cached_avis_.size() >=
                         policy_.fp.batch.unsync_base_limit))
            {
                drain();
                DCHECK(cached_avis_.empty());
            }
        }
        // TODO: not good but currently works
        // a free should clear polled_block to reduce *false*
        // maybe_exhausted.
        auto &c = alloc_ctx;
        c.polled_block = 0;
    }

    // directly write to release is okay
    void free_huge(const AvisAddress &addr)
    {
        [[maybe_unused]] auto &c = free_ctx;

        auto lay = layout();

        auto [block_id, slot_id, _] =
            pool_view_.offset_to_position(addr.offset());
        uint64_t all_bitmap_raddr =
            (uint64_t) pool_view_.ith_block_bitmap_meta_addr(block_id);
        auto bitmap_sz = lay.block_bitmap_entry_size();
        uint64_t cur_bitmap_raddr = all_bitmap_raddr + slot_id * bitmap_sz;

        char *p_bitmap = (char *) c.locate_bitmap(lay, slot_id);

        std::vector<char> add_val(bitmap_sz, 0xff);
        const auto &field_boundary = add_val;

        c.cached_central_bitmap->assert_contains((void *) p_bitmap, bitmap_sz);
        // NOTE: use write for better performance
        // but we use faa to enable debug
        dsm_->prepare_faa(node_id_,
                          rkey_,
                          cur_bitmap_raddr,
                          bitmap_sz /* size */,
                          (uint64_t) add_val.data(),
                          (uint64_t) field_boundary.data(),
                          (void *) p_bitmap,
                          ctx_);
        m_.faa(bitmap_sz);

        dsm_->commit(ctx_);
        auto bitmap_bits = util::BitsViewMut(p_bitmap, bitmap_sz);
        if constexpr (debug())
        {
            CHECK(bitmap_bits.all())
                << "** Corruption detected: freeing a huge page "
                   "with bitmap not `all-ones`"
                << std::endl
                << bitmap_bits.hex_dump();
        }

        // maintain
        bitmap_bits.reset();
    }
    // block_id, slot_id, frame_id
    auto position_from_offset(uint32_t offset)
    {
        return pool_view_.offset_to_position(offset);
    }

    void update_meta()
    {
        LOG(FATAL) << "TODO...";
    }

    /**
     * issue CAS once.
     */
    bool try_allocate_huge_once()
    {
        auto &c = alloc_ctx;
        auto block_id = *c.select_block_idx;
        auto slot_id = *c.select_slot_idx;

        auto lay = layout();

        uint64_t all_bitmap_raddr =
            (uint64_t) pool_view_.ith_block_bitmap_meta_addr(block_id);
        auto bitmap_sz = lay.block_bitmap_entry_size();
        uint64_t cur_bitmap_raddr = all_bitmap_raddr + slot_id * bitmap_sz;

        // expect all zeros
        std::vector<char> expect(bitmap_sz, 0);
        std::vector<char> mask(bitmap_sz, 0xff);

        char *p_bitmap = (char *) c.locate_bitmap(lay, slot_id);
        auto bitmap_bits = util::BitsViewMut(p_bitmap, bitmap_sz);
        c.cached_central_bitmap->assert_contains(p_bitmap, bitmap_sz);
        if constexpr (debug())
        {
            if (unlikely(bitmap_bits !=
                         util::BitsView(expect.data(), expect.size())))
            {
                LOG(FATAL)
                    << "** Why bring me here? Selected " << PRE(c)
                    << " for full huge page but the bitmap is not all zeros.";
            }
        }

        // NOTE: no need to retry
        // We must CAS the entire 32 byte zeros to entire 32 byte ones.
        // for atomic huge-page allocation
        dsm_->prepare_cas(node_id_,
                          rkey_,
                          cur_bitmap_raddr,
                          bitmap_sz /* size */,
                          (uint64_t) p_bitmap /* compare */,
                          (uint64_t) mask.data() /* compare mask */,
                          (uint64_t) mask.data() /* swap */,
                          (uint64_t) mask.data() /* swap mask */,
                          (void *) p_bitmap,
                          ctx_);
        dsm_->commit(ctx_);
        if (memcmp((void *) p_bitmap, expect.data(), bitmap_sz) == 0)
        {
            // set p_bitmap to the after value
            const auto &swap = mask;
            // NOTE: Needs a bswap in fact,
            // but swap.data() is all zeros here, so no need.
            memcpy(p_bitmap, swap.data(), bitmap_sz);
            return true;
        }
        return false;
    }

    uint32_t to_offset(size_t block_id, size_t slot_id)
    {
        return pool_view_.huge_frame_offset(block_id, slot_id);
    }
    uint32_t to_offset(size_t block_id, size_t slot_id, size_t page_id)
    {
        return pool_view_.base_frame_offset(block_id, slot_id, page_id);
    }

    AvisAddress avis_address_from_offset(uint32_t offset) const
    {
        auto ret = AvisAddress(node_id_, region_id_, offset);
        return ret;
    }

    AvisAddress do_alloc_huge()
    {
        auto &c = alloc_ctx;

        while (likely(!maybe_exhausted_))
        {
            auto state = select_next_huge();
            if (unlikely(state == kFull))
            {
                return avis_address_from_offset(0);
            }

            bool succ = try_allocate_huge_once();
            if (succ)
            {
                auto block_id = *c.select_block_idx;
                auto slot_id = *c.select_slot_idx;
                LOG_IF(INFO, kReportAlloc) << "[alloc] huge: block " << block_id
                                           << " slot " << slot_id;
                uint32_t offset = to_offset(block_id, slot_id);
                return avis_address_from_offset(offset);
            }
        }
        return avis_address_from_offset(0);
    }
    ~AvisPoolAllocator()
    {
        if (alloc_ctx.cached_central_bitmap)
        {
            dsm_->put_rdma_buffer(std::move(*alloc_ctx.cached_central_bitmap));
        }
        if (free_ctx.cached_central_bitmap)
        {
            dsm_->put_rdma_buffer(std::move(*free_ctx.cached_central_bitmap));
        }
    }
    RemoteMetrics remote_metrics() override
    {
        return m_;
    }

    Layout layout() const
    {
        return Layout(conf_);
    }

    // Refresh local status to respect remote status
    void refresh()
    {
        LOG(WARNING) << "Doing refresh!!";
        drain();
        DCHECK(cached_avis_.empty());
        maybe_exhausted_ = false;
        init_alloc_ctx();
    }

private:
    DSM::pointer dsm_;
    uint16_t node_id_;
    uint16_t region_id_;
    uint32_t rkey_;
    const Config &conf_;
    CoroContext *ctx_;
    [[maybe_unused]] const Policy &policy_;

    PoolView pool_view_;

    RemoteMetrics m_;

    std::stack<avis::AvisAddress> cached_avis_;

    bool maybe_exhausted_{false};

    enum State
    {
        kEmpty = 0b00,
        kPartial = 0b01,
        kReserved = 0b10,
        kFull = 0b11,
    };

    AllocCtx alloc_ctx;
    FreeCtx free_ctx;

    // CONTRACT:
    // When returned, c.select_[block|slot|page]_id is okay.
    State select_next_base()
    {
        auto &c = alloc_ctx;
        if (unlikely(maybe_exhausted_))
        {
            return kFull;
        }
        if (unlikely(!c.select_block_idx || !c.select_slot_idx ||
                     !c.select_page_idx))
        {
            init_alloc_ctx();
        }
        return select_next_block(false /* huge */);
    }
    State select_next_huge()
    {
        auto &c = alloc_ctx;
        if (unlikely(maybe_exhausted_))
        {
            return kFull;
        }
        if (unlikely(!c.select_block_idx || !c.select_slot_idx ||
                     !c.select_page_idx))
        {
            init_alloc_ctx();
        }
        return select_next_block(true /* huge */);
    }

    bool exhausted() const
    {
        return maybe_exhausted_;
    }

    State select_next_block(bool huge)
    {
        auto &c = alloc_ctx;
        while (true)
        {
            auto block_nr = conf_.block_nr_per_pool;
            auto state = select_next_slot(huge);
            if (likely(state != kFull))
            {
                return state;
            }
            else
            {
                // switch to a new block

                c.select_block_idx = (*c.select_block_idx + 1) % block_nr;
                // c.select_slot_idx = 0;
                // LOG(INFO) << "Use new block: " << *c.select_block_idx
                //           << " polled " << c.polled_block << " / " <<
                //           block_nr;
                if (unlikely(++c.polled_block > block_nr))
                {
                    // LOG(INFO) << "exhausted";
                    maybe_exhausted_ = true;
                    return kFull;
                }
                // remember to clear the cached meta
                auto block_meta_size = layout().block_meta_size();
                DCHECK_GE(c.cached_central_bitmap->size, block_meta_size);
                memset(c.cached_central_bitmap->buffer, 0, block_meta_size);
            }
        }
    }
    State select_next_slot(bool huge)
    {
        auto &c = alloc_ctx;
        // TODO: revoke back the +1 here
        // auto cur_slot_id = *c.select_slot_idx;
        // auto block_id = *c.select_block_idx;
        // const char *tag = huge ? "huge" : "base";

        auto slot_nr = conf_.huge_frame_nr_per_block;
        auto bitmap_sz = layout().block_bitmap_entry_size();

        // NOTE: don't use pcentral anymore.
        for (size_t i = 0; i < slot_nr; ++i)
        {
            size_t slot_id = *c.select_slot_idx;
            auto next_slot_id = (slot_id + 1) % slot_nr;
            if (unlikely(*c.select_block_idx == 0 && slot_id == 0) && huge)
            {
                // don't give out nullptr
                *c.select_slot_idx = next_slot_id;
                continue;
            }

            char *bitmap_addr = (char *) c.locate_bitmap(layout(), slot_id);
            auto avail_nr = util::count_zeros(bitmap_addr, bitmap_sz);

            if (avail_nr == 0)
            {
                LOG_IF(INFO, kReportSelect)
                    << "[avis-batch] [rej] NOT avail: " << PRE(c);
                // whole slot not avaiable
                *c.select_slot_idx = next_slot_id;
                continue;
            }
            else if (avail_nr == bitmap_sz * util::bit_nr<uint8_t>())
            {
                LOG_IF(INFO, kReportSelect)
                    << "[avis-batch] [select] ALL avail: " << PRE(c);
                *c.select_page_idx = 0;
                return kEmpty;
            }
            else
            {
                if (huge)
                {
                    continue;
                }
                else
                {
                    // partial
                    auto avail_idx =
                        util::BitsView(bitmap_addr, bitmap_sz).firstr_unset();
                    DCHECK_LE(avail_idx,
                              bitmap_sz * util::bit_nr<uint8_t>() - 1);
                    LOG_IF(INFO, kReportSelect)
                        << "[avis-batch] [select] partial avail: "
                        << PRE(c, avail_nr, avail_idx) << std::endl
                        << util::Hexdump(bitmap_addr, bitmap_sz);
                    *c.select_page_idx = avail_idx;
                    return kPartial;
                }
            }
        }

        return kFull;
    }

    State select_next_page()
    {
        auto &c = alloc_ctx;

        auto block_id = *c.select_block_idx;
        auto slot_id = *c.select_slot_idx;
        char *p_bitmap = (char *) (c.cached_central_bitmap->buffer +
                                   layout().block_central_size());
        uint8_t *p_cur_bitmap =
            (uint8_t *) (p_bitmap +
                         slot_id * layout().block_bitmap_entry_size());

        auto entry_nr = layout().entry_nr_per_bitmap();
        for (size_t i = 0; i < entry_nr; ++i)
        {
            auto page_id = *c.select_page_idx;
            auto next_page_id = (page_id + 1) % entry_nr;

            auto byte_id = page_id / 8;
            auto bit_offset = page_id % 8;
            uint8_t byte = p_cur_bitmap[byte_id];
            // TODO: if byte are all zeros
            // fail fast: don't check every bit sequentially.
            if (byte & (1ull << bit_offset))
            {
                // allocated
                // LOG(INFO) << "[refuse] block: " << *c.select_block_idx
                //           << " slot " << *c.select_slot_idx << " page "
                //           << cur_page_id << " byte " <<
                //           util::pre_bin(byte)
                //           << " from byte_id " << byte_id
                //           << " offset: " << bit_offset << " at "
                //           << (void *) &p_cur_bitmap[byte_id];
                *c.select_page_idx = next_page_id;
            }
            else
            {
                // unallocated
                LOG_IF(INFO, kReportSelect)
                    << "[alloc-select] base: block " << block_id << " slot "
                    << slot_id << " page " << page_id << " because byte is "
                    << util::pre_bin(byte) << " from byte_id " << byte_id
                    << " offset: " << bit_offset << " at "
                    << (void *) &p_cur_bitmap[byte_id];

                return kEmpty;
            }
        }
        // LOG(INFO) << "!!! page drained";
        return kFull;
    }

    void init_alloc_ctx()
    {
        auto &c = alloc_ctx;
        auto block_nr = conf_.block_nr_per_pool;
        c.select_block_idx = fast_pseudo_rand_int(0, block_nr - 1);
        c.select_slot_idx = 0;
        c.select_page_idx = 0;

        auto block_meta_size = layout().block_meta_size();
        auto buffer = dsm_->get_rdma_buffer(block_meta_size);
        memset(buffer.buffer, 0, block_meta_size);
        c.cached_central_bitmap.emplace(std::move(buffer));

        c.polled_block = 0;
    }
};

inline std::ostream &operator<<(std::ostream &os, const AvisPoolAllocator &a)
{
    std::ignore = a;
    os << "batch::AvisPoolAllocator";
    return os;
}

}  // namespace avis::policy::batch