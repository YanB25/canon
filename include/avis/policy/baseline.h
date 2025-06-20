#pragma once
#include <cinttypes>

#include "./api.h"
#include "DSM.h"
#include "Metrics.h"
#include "avis/layout.h"
#include "avis/policy.h"
#include "memory/allocator.h"
#include "util/bits.h"

namespace avis::policy::baseline
{
struct AllocCtx
{
    std::optional<uint32_t> select_block_idx;
    std::optional<uint32_t> select_slot_idx;
    std::optional<uint32_t> select_page_idx;  // only for base frame
    // this is per-block:
    // when switching block, clear me or read me.
    std::optional<Buffer> cached_central_bitmap;
    size_t polled_block = 0;
};

struct FreeCtx
{
    std::optional<Buffer> cached_central_bitmap;
    std::optional<size_t> block_id;
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

    AvisPoolAllocator(DSM::pointer dsm,
                      uint16_t node_id,
                      uint16_t region_id,
                      uint32_t rkey,
                      void *pool_meta_addr,
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
    AvisAddress alloc(size_t size) override
    {
        if (size > conf_.base_frame_size)
        {
            DLOG_IF(WARNING, size > conf_.huge_frame_size)
                << "** too huge to allocate: " << size;
            return alloc_huge();
        }
        else
        {
            return alloc_base();
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
            free_base(addr);
        }
        // TODO: not good but currently works
        // a free should clear polled_block to reduce *false*
        // maybe_exhausted.
        auto &c = alloc_ctx;
        c.polled_block = 0;
    }

    void free_huge(const AvisAddress &addr)
    {
        auto &c = free_ctx;

        auto [block_id, slot_id, _] =
            pool_view_.offset_to_position(addr.offset());
        auto word_id = slot_id / 32;
        auto slot_offset = slot_id % 32;
        auto bit_offset = slot_offset * 2;  // 2 b

        uint64_t central_addr =
            (uint64_t) pool_view_.ith_block_central_meta_addr(block_id);

        volatile uint64_t *pcached_word =
            (volatile uint64_t *) c.cached_central_bitmap->buffer;
        volatile uint64_t *pbuffer = &pcached_word[word_id];
        uint64_t add_val = util::ones<uint64_t>(bit_offset, bit_offset + 1);
        uint64_t field_boundary = add_val;
        uint64_t remote_addr = central_addr + 8 * word_id;

        dsm_->prepare_faa(node_id_,
                          rkey_,
                          remote_addr,
                          8,
                          add_val,
                          field_boundary,
                          (void *) pbuffer,
                          ctx_);
        dsm_->commit(ctx_);
        uint64_t org = *pbuffer;
        CHECK_EQ(org & field_boundary, add_val)
            << "** expect central to be 0b11, got " << util::pre_bin(org)
            << ", masked: " << util::pre_bin(org & field_boundary);

        // update pbuffer to reflect that
        *pbuffer = (org ^ add_val);
    }
    void free_base(const AvisAddress &addr)
    {
        auto &c = free_ctx;
        auto lay = layout();

        auto [block_id, slot_id, page_id] =
            pool_view_.offset_to_position(addr.offset());

        // TODO: freeing a page does not try to change the central back to
        // 0b00 therefore, huge-page alloc can not leverage this block.

        // uint64_t central =
        //     (uint64_t) pool_view_.ith_block_central_meta_addr(block_id);
        uint64_t bitmap =
            (uint64_t) pool_view_.ith_block_bitmap_meta_addr(block_id);

        char *p_bitmap =
            (c.cached_central_bitmap->buffer + lay.block_central_size());
        char *p_cur_bitmap = p_bitmap + slot_id * lay.block_bitmap_entry_size();
        uint64_t *p_cur_bitmap_word = (uint64_t *) p_cur_bitmap;

        auto bit_nr_per_word = sizeof(uint64_t) * 8;
        auto word_id = page_id / bit_nr_per_word;
        auto bit_offset = page_id % bit_nr_per_word;

        uint64_t cur_bitmap_addr =
            bitmap + slot_id * lay.block_bitmap_entry_size();
        uint64_t remote_addr = cur_bitmap_addr + word_id * 8;
        volatile uint64_t *pbuffer = &p_cur_bitmap_word[word_id];

        uint64_t add_val = util::one_at<uint64_t>(bit_offset);
        uint64_t field_boundary = add_val;

        LOG_IF(INFO, kReportRDMA)
            << "[free] page FAA-boundary slot: " << slot_id
            << ", page: " << page_id << ", bit_offset: " << bit_offset;

        dsm_->prepare_faa(node_id_,
                          rkey_,
                          remote_addr,
                          8,
                          add_val,
                          field_boundary,
                          (void *) pbuffer,
                          ctx_);
        dsm_->commit(ctx_);
        uint64_t org = *pbuffer;
        CHECK_EQ(org & field_boundary, add_val)
            << "** expect bitmap to be a set bit at " << PRE(bit_offset)
            << ", got " << util::pre_bin(org)
            << " masked: " << util::pre_bin(org & field_boundary);

        *pbuffer = (org ^ add_val);
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

    bool try_allocate_base_once(uint64_t bitmap_addr, CoroContext *ctx)
    {
        auto &c = alloc_ctx;
        auto lay = layout();

        auto block_id = *c.select_block_idx;
        auto slot_id = *c.select_slot_idx;
        auto page_id = *c.select_page_idx;

        char *p_bitmap =
            (c.cached_central_bitmap->buffer + lay.block_central_size());
        char *p_cur_bitmap = p_bitmap + slot_id * lay.block_bitmap_entry_size();
        uint64_t *p_cur_bitmap_word = (uint64_t *) p_cur_bitmap;

        auto bit_nr_per_word = sizeof(uint64_t) * 8;
        auto word_id = page_id / bit_nr_per_word;
        auto bit_offset = page_id % bit_nr_per_word;

        uint64_t cur_bitmap_addr =
            bitmap_addr + slot_id * lay.block_bitmap_entry_size();
        uint64_t remote_addr = cur_bitmap_addr + word_id * 8;
        volatile uint64_t *pcompare = &p_cur_bitmap_word[word_id];

        uint64_t compare = *pcompare;
        uint64_t compare_mask = util::one_at<uint64_t>(bit_offset);
        uint64_t swap_mask = compare_mask;
        uint64_t swap = swap_mask;

        LOG_IF(INFO, kReportRDMA)
            << "[alloc] CAS slot: " << slot_id << ", page: " << page_id
            << ", bit_offset: " << bit_offset
            << ", compare_mask: " << (void *) compare_mask;

        if (unlikely((compare & compare_mask) != 0))
        {
            LOG(FATAL) << "Already allocated, why you bring me here? compare: "
                       << util::pre_bin(compare)
                       << ", compare_mask: " << util::pre_bin(compare_mask)
                       << ", bit_offset: " << bit_offset << " from block "
                       << block_id << " slot " << slot_id << " page "
                       << page_id;
            return false;
        }

        dsm_->prepare_cas(node_id_,
                          rkey_,
                          remote_addr,
                          8,
                          compare,
                          compare_mask,
                          swap,
                          swap_mask,
                          (void *) pcompare,
                          ctx);
        dsm_->commit(ctx);

        uint64_t old_val = *pcompare;
        if ((old_val & compare_mask) == (compare & compare_mask))
        {
            // succeeded
            uint64_t new_val = old_val;
            new_val &= ~compare_mask;  // clear mask
            new_val |= swap & swap_mask;
            *pcompare = new_val;
            m_.cas(8, true /* ok */);
            LOG_IF(INFO, kReportRDMA)
                << "CAS succeeded: old_val: " << util::pre_bin(old_val)
                << ", new_val: " << util::pre_bin(new_val) << ", compare_mask "
                << util::pre_bin(compare_mask)
                << ", swap: " << util::pre_bin(swap)
                << ", swap_mask: " << util::pre_bin(swap_mask);

            return true;
        }
        else
        {
            // LOG(INFO) << "CAS failed: old_val: " <<
            // util::pre_bin(old_val)
            //           << ", compare: " << util::pre_bin(compare)
            //           << ", c_mask: " << util::pre_bin(compare_mask)
            //           << ", at block " << block_id << " slot " << slot_id
            //           << " page " << page_id << " at " << (void *)
            //           pcompare;

            m_.cas(8, false /* ok */);
            return false;
        }
    }

    /**
     * issue CAS once.
     */
    bool try_allocate_huge_once(uint64_t central_addr,
                                uint32_t slot_id,
                                CoroContext *ctx)
    {
        auto &c = alloc_ctx;
        auto word_id = slot_id / 32;
        auto slot_offset = slot_id % 32;
        auto bit_offset = slot_offset * 2;  // 2 b

        volatile uint64_t *pcached_word =
            (volatile uint64_t *) c.cached_central_bitmap->buffer;

        volatile uint64_t *pcompare = &pcached_word[word_id];
        uint64_t compare = *pcompare;
        uint64_t compare_mask =
            util::ones<uint64_t>(bit_offset, bit_offset + 1);  // 2b
        uint64_t swap = compare_mask;
        uint64_t swap_mask = compare_mask;
        uint64_t remote_addr = central_addr + 8 * word_id;

        // LOG(INFO) << "before CAS: get " << util::pre_bin(compare);
        dsm_->prepare_cas(node_id_,
                          rkey_,
                          remote_addr,
                          8,
                          compare,
                          compare_mask,
                          swap,
                          swap_mask,
                          (void *) pcompare,
                          ctx);
        dsm_->commit(ctx);

        uint64_t old_val = *pcompare;
        if ((old_val & compare_mask) == (compare & compare_mask))
        {
            // succeeded
            uint64_t new_val = old_val;
            new_val &= ~compare_mask;  // clear mask
            new_val |= swap & swap_mask;
            *pcompare = new_val;

            m_.cas(8, true /* ok */);
            // LOG(INFO) << "OK after CAS " << std::endl
            //           << util::pre_bin(old_val) << " => "
            //           << util::pre_bin(new_val);
            return true;
        }
        else
        {
            m_.cas(8, false /* ok */);
            // LOG(INFO) << "FAILED old_val: " << util::pre_bin(old_val);
            return false;
        }
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
        return AvisAddress(node_id_, region_id_, offset);
    }

    AvisAddress alloc_huge()
    {
        // each slot is 2b
        // each byte has 4 slots
        // each 8 byte (atomic unit) has 32 slots

        auto &c = alloc_ctx;

        while (likely(!maybe_exhausted_))
        {
            auto state = select_next_huge();
            if (unlikely(state == kFull))
            {
                return avis_address_from_offset(0);
            }
            auto block_id = *c.select_block_idx;
            auto slot_id = *c.select_slot_idx;
            uint64_t central =
                (uint64_t) pool_view_.ith_block_central_meta_addr(block_id);

            bool succ = try_allocate_huge_once(central, slot_id, ctx_);
            if (succ)
            {
                LOG_IF(INFO, kReportAlloc) << "[alloc] huge: block " << block_id
                                           << " slot " << slot_id;
                uint32_t offset = to_offset(block_id, slot_id);
                return avis_address_from_offset(offset);
            }
        }
        return avis_address_from_offset(0);
    }
    AvisAddress alloc_base()
    {
        auto &c = alloc_ctx;
        while (likely(!maybe_exhausted_))
        {
            auto state = select_next_base();
            if (unlikely(state == kFull))
            {
                return avis_address_from_offset(0);
            }
            auto block_id = *c.select_block_idx;
            auto slot_id = *c.select_slot_idx;
            auto page_id = *c.select_page_idx;
            uint64_t central =
                (uint64_t) pool_view_.ith_block_central_meta_addr(block_id);
            uint64_t bitmap =
                (uint64_t) pool_view_.ith_block_bitmap_meta_addr(block_id);
            bool succ = try_lock_central_once(central, slot_id, ctx_);
            if (!succ)
            {
                // lock failed, maybe allocated as a whole huge
                continue;
            }
            succ = try_allocate_base_once(bitmap, ctx_);
            if (succ)
            {
                LOG_IF(INFO, kReportAlloc)
                    << "[alloc] base: block " << block_id << " slot " << slot_id
                    << " page " << page_id;

                auto offset = to_offset(block_id, slot_id, page_id);
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

    bool try_lock_central_once(uint64_t central_addr,
                               uint32_t slot_id,
                               CoroContext *ctx)
    {
        auto &c = alloc_ctx;
        auto word_id = slot_id / 32;
        auto slot_offset = slot_id % 32;
        auto bit_offset = slot_offset * 2;  // 2 b

        volatile uint64_t *pcached_word =
            (volatile uint64_t *) c.cached_central_bitmap->buffer;
        uint64_t remote_addr = central_addr + 8 * word_id;
        volatile uint64_t *pcompare = &pcached_word[word_id];
        uint64_t compare = *pcompare;
        uint64_t compare_mask =
            util::ones<uint64_t>(bit_offset, bit_offset + 1);  // 2b
        uint64_t swap_mask = compare_mask;
        // make it partial
        uint64_t swap = (uint64_t) kPartial << bit_offset;
        DCHECK_NE(swap & swap_mask, 0)
            << "** please check if it is correct:"
            << "swap: " << util::pre_bin(swap)
            << ", swap_mask: " << util::pre_bin(swap_mask)
            << ", &: " << util::pre_bin(swap & swap_mask);

        uint64_t locked_tag_swap = (uint64_t) kFull << bit_offset;
        if (unlikely((compare & compare_mask) == (swap & swap_mask)))
        {
            // Already locked. No action.
            return true;
        }
        if (unlikely((compare & compare_mask) == (locked_tag_swap & swap_mask)))
        {
            LOG(FATAL) << "[RDMA] cached central indicates full, not possible "
                          "for base allocation. "
                       << "block_id: " << *c.select_block_idx
                       << " slot_id: " << *c.select_slot_idx
                       << " page _id: " << *c.select_page_idx
                       << " details: " << PRE(word_id) << ", "
                       << PRE(slot_offset) << ", " << PRE(bit_offset);
            return false;
        }

        dsm_->prepare_cas(node_id_,
                          rkey_,
                          remote_addr,
                          8,
                          compare,
                          compare_mask,
                          swap,
                          swap_mask,
                          (void *) pcompare,
                          ctx);
        dsm_->commit(ctx);

        uint64_t old_val = *pcompare;
        if ((old_val & compare_mask) == (compare & compare_mask))
        {
            // succeeded
            uint64_t new_val = old_val;
            new_val &= ~compare_mask;  // clear mask
            new_val |= swap & swap_mask;
            *pcompare = new_val;
            m_.cas(8, true /* ok */);
            return true;
        }
        else if ((old_val & compare_mask) == (swap & swap_mask))
        {
            // the old value is what we want
            m_.cas(8, false /* ok */);
            return true;
        }
        else
        {
            m_.cas(8, false /* ok */);
            return false;
        }
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
                memset(c.cached_central_bitmap->buffer, 0, block_meta_size);
            }
        }
    }
    State select_next_slot(bool huge)
    {
        auto &c = alloc_ctx;
        // TODO: revoke back the +1 here
        // auto cur_slot_id = *c.select_slot_idx;
        auto block_id = *c.select_block_idx;
        const char *tag = huge ? "huge" : "base";

        auto slot_nr = conf_.huge_frame_nr_per_block;

        uint8_t *pcentral = (uint8_t *) c.cached_central_bitmap->buffer;
        for (size_t i = 0; i < slot_nr; ++i)
        {
            // size_t slot_id = (cur_slot_id + i) % slot_nr;
            size_t slot_id = *c.select_slot_idx;
            auto next_slot_id = (slot_id + 1) % slot_nr;
            if (unlikely(*c.select_block_idx == 0 && slot_id == 0) && huge)
            {
                // don't give out nullptr
                *c.select_slot_idx = next_slot_id;
                continue;
            }
            auto byte_id = slot_id / 4;
            auto slot_offset = slot_id % 4;
            uint8_t byte = pcentral[byte_id];
            auto bits =
                util::get_rng_shift(byte, slot_offset * 2, slot_offset * 2 + 1);
            if (bits == 0b00)
            {
                LOG_IF(INFO, kReportSelect)
                    << "[alloc-select] " << tag << ": EMPTY block "
                    << PRE(block_id) << " " << PRE(slot_id) << " because "
                    << util::pre_bin(byte) << " at byte " << byte_id << " off "
                    << slot_offset;

                c.select_page_idx = 0;
                return kEmpty;
            }
            else if (bits == 0b11)
            {
                // LOG(INFO) << "FULL " << PRE(slot_id) << " / " << slot_nr
                //           << " under " << util::pre_bin(byte) << " at
                //           byte "
                //           << byte_id << " off " << slot_offset;
                *c.select_slot_idx = next_slot_id;
            }
            else
            {
                // it is partial
                LOG_IF(INFO, kReportSelect)
                    << "[alloc-select] " << tag << ": PARTIAL block "
                    << PRE(block_id) << " " << PRE(slot_id) << " because "
                    << util::pre_bin(byte) << " at byte " << byte_id << " off "
                    << slot_offset;

                if (!huge)
                {
                    auto state = select_next_page();
                    if (likely(state != kFull))
                    {
                        return state;
                    }
                }
                *c.select_slot_idx = next_slot_id;
            }
        }
        // LOG(INFO) << "!!! slot drained";
        return kFull;
    }

    State select_next_page()
    {
        auto &c = alloc_ctx;
        auto lay = layout();

        auto block_id = *c.select_block_idx;
        auto slot_id = *c.select_slot_idx;
        char *p_bitmap = (char *) (c.cached_central_bitmap->buffer +
                                   lay.block_central_size());
        uint8_t *p_cur_bitmap =
            (uint8_t *) (p_bitmap + slot_id * lay.block_bitmap_entry_size());

        auto entry_nr = lay.entry_nr_per_bitmap();
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
    os << "AvisPoolAllocator";
    return os;
}

}  // namespace avis::policy::baseline