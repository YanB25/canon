#pragma once

#include <cinttypes>
#include <iostream>
#include <memory>

#include "./bitmap_policy.h"
#include "Common.h"
#include "avis/sizeclass.h"
#include "util/Pre.h"
#include "util/Util.h"

namespace avis
{
enum BuddyMode
{
    kBoundedRand,
    kRand,
    kSeq,
};

class Config
{
public:
    constexpr static bool kReportBP = false;
    constexpr static bool kReportPTL = false;
    static Config &ins()
    {
        static Config conf_;
        return conf_;
    }

    void configure_ptl(bool enable)
    {
        enable_ptl_ = enable;
    }
    void configure_bp(bool enable)
    {
        enable_bp_ = enable;
    }
    void configure_bitmap_degree(int degree)
    {
        CHECK_LE(degree, 6) << "** can not be higher";
        bitmap_policy_ = std::make_unique<avis::OrderedBitmapSize>(degree);
    }
    void configure_fixed_bitmap_size(size_t size)
    {
        bitmap_policy_ = std::make_unique<avis::FixedBitmapSize>(size);
    }
    void configure_bitmap_size_custom()
    {
        bitmap_policy_ = std::make_unique<avis::CustomBitmapSize>();
    }
    void configure_share_bitmap(bool en)
    {
        bitmap_policy_->configure_share_bitmap(en);
    }
    void configure_cache_fetch_block_nr(size_t nr)
    {
        cache_fetch_block_nr_ = nr;
    }
    void configure_use_partition_allocator(bool use)
    {
        use_partition_allocator_ = use;
    }
    void configure_use_buddy_ge(size_t sz)
    {
        use_buddy_ge_ = sz;
    }
    void simulate_crash()
    {
        buddy_has_crashed_ = true;
    }
    bool buddy_has_crashed() const
    {
        return buddy_has_crashed_;
    }

    size_t cache_fetch_block_nr() const
    {
        return cache_fetch_block_nr_;
    }

    bool enable_ptl() const
    {
        return enable_ptl_;
    }
    bool enable_bp() const
    {
        return enable_bp_;
    }

    bool share_bitmap([[maybe_unused]] size_t object_size) const
    {
        return bitmap_policy_->enable_share_bitmap(object_size);
    }

    BuddyMode buddy_mode() const
    {
        return buddy_mode_;
    }

    constexpr bool use_buddy(size_t size) const
    {
        return size >= use_buddy_ge_;
    }

    /**
     * It turns out that the cache's upper mark and expect size does not matter:
     * In most cases, there are lots of cache (e.g., 100-200),
     * where in each cache containing one or several objects.
     */
    size_t cache_upper_mark(size_t object_size) const
    {
        return bitmap_policy_->cache_upper_mark(object_size);
    }
    size_t cache_expect_size(size_t object_size) const
    {
        return bitmap_policy_->cache_expect_size(object_size);
    }

    bool strict_cache_size(size_t object_size) const
    {
        return bitmap_policy_->strict_cache_size(object_size);
    }

    constexpr bool use_slab(size_t size) const
    {
        return !use_buddy(size);
    }
    size_t to_class_size(size_t size) const
    {
        if (unlikely(size == 0))
        {
            LOG(FATAL) << "check that.";
        }
        return avis::SizeClass{}.to_class_size(size);
    }
    /**
     * bitmap_size returns the expected size of bitmap.
     * @param size the size of the object in bitmap
     */
    size_t bitmap_size(size_t size) const
    {
        auto ret = bitmap_policy_->bitmap_size(size);
        // bitmap size has to be 2^n pages. round up here.
        auto page_size = 4_KB;
        auto page_nr = std::max(ret / page_size, 1ul);
        page_nr = util::round_up_next_power_of_two(page_nr);
        return page_nr * page_size;
    }

    size_t partition_size() const
    {
        return partition_size_;
    }
    void configure_partition_size(size_t size)
    {
        partition_size_ = size;
    }

    bool use_partition_allocator() const
    {
        return use_partition_allocator_;
    }
    bool buddy_remember_failure() const
    {
        return buddy_remember_failure_;
    }
    bool buddy_use_postorder() const
    {
        return buddy_use_postorder_;
    }
    void configure_buddy_use_post_order(bool use)
    {
        buddy_use_postorder_ = use;
    }
    void configure_buddy_mode(BuddyMode mode)
    {
        buddy_mode_ = mode;
    }

    bool use_rpc_allocator() const
    {
        return use_rpc_allocator_;
    }
    void set_use_rpc_allocator(bool b)
    {
        use_rpc_allocator_ = b;
    }

    void configure_drop_deallocated_obj(bool en)
    {
        drop_deallocated_obj_ = en;
    }
    bool drop_deallocated_obj() const
    {
        return drop_deallocated_obj_;
    }

private:
    bool enable_ptl_{false};
    bool enable_bp_{false};
    bool buddy_use_postorder_{true};
    bool buddy_has_crashed_{false};
    bool use_rpc_allocator_{false};

    size_t partition_size_{512_MB};

    uint64_t use_buddy_ge_{32_KB};

    std::unique_ptr<avis::IBitmapPolicy> bitmap_policy_{
        std::make_unique<OrderedBitmapSize>(3)};

    BuddyMode buddy_mode_{BuddyMode::kBoundedRand};

    bool use_partition_allocator_{false};

    size_t cache_fetch_block_nr_{1};

    bool drop_deallocated_obj_{false};

    bool buddy_remember_failure_{true};

    Config() = default;
};

}  // namespace avis