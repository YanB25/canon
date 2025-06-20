#include <cmath>
#include <cstddef>

#include "util/Literals.h"
#include "util/Util.h"
using namespace util::literals;

namespace avis
{
class IBitmapPolicy
{
public:
    virtual size_t bitmap_size(size_t object_size) const = 0;
    virtual bool enable_share_bitmap(size_t object_size) const = 0;
    virtual void configure_share_bitmap(bool en) = 0;
    virtual size_t cache_upper_mark([[maybe_unused]] size_t object_size) const
    {
        return 1 * 64 + 8;
    }
    virtual size_t cache_expect_size([[maybe_unused]] size_t object_size) const
    {
        return 32;
    }
    virtual bool strict_cache_size([[maybe_unused]] size_t object_size) const
    {
        return false;
    }
    virtual ~IBitmapPolicy() = default;
};

class FixedBitmapSize : public IBitmapPolicy
{
public:
    FixedBitmapSize(size_t size) : size_(size)
    {
    }
    size_t bitmap_size(size_t) const override
    {
        return size_;
    }
    bool enable_share_bitmap(size_t) const override
    {
        return en_share_;
    }
    void configure_share_bitmap(bool en) override
    {
        en_share_ = en;
    }

private:
    size_t size_;
    bool en_share_{false};
};

class OrderedBitmapSize : public IBitmapPolicy
{
public:
    OrderedBitmapSize(size_t degree) : bitmap_degree_(degree)
    {
    }
    size_t bitmap_size(size_t size) const override
    {
        auto tmp = ceil(log2(size));
        unsigned order_4kb = tmp >= bitmap_degree_ ? tmp - bitmap_degree_ : 0;
        auto ret = 4_KB * (1ull << order_4kb);
        return ret;
    }
    bool enable_share_bitmap(size_t) const override
    {
        return en_share_;
    }
    void configure_share_bitmap(bool en) override
    {
        en_share_ = en;
    }

private:
    size_t bitmap_degree_;
    bool en_share_{true};
};

class CustomBitmapSize : public IBitmapPolicy
{
public:
    CustomBitmapSize()
    {
    }
    size_t bitmap_size(size_t size) const override
    {
        if (size >= 1_MB)
        {
            return 32_MB;
        }
        if (size >= 100_KB)
        {
            return 2_MB;
        }
        if (size <= 529)
        {
            return std::min(2_MB, 2_K * size);
        }
        return std::min(4_MB, 16_K * size);
    }
    bool enable_share_bitmap(size_t object_size) const override
    {
        if (object_size >= 100_KB)
        {
            return false;
        }
        return true;
    }
    void configure_share_bitmap(bool) override
    {
        // drop
    }
    size_t cache_upper_mark([[maybe_unused]] size_t object_size) const override
    {
        if (object_size >= 1_MB)
        {
            return 1;
        }
        if (object_size >= 100_KB)
        {
            return 2;
        }
        if (object_size >= 50_KB)
        {
            return 4;
        }
        if (object_size >= 1_KB)
        {
            return 16;
        }
        // if (object_size <= 529)
        // {
        //     return 3 * 64;
        // }
        return 1 * 64 + 8;
    }
    size_t cache_expect_size([[maybe_unused]] size_t object_size) const override
    {
        if (object_size >= 1_MB)
        {
            return 1;
        }
        if (object_size >= 100_KB)
        {
            return 2;
        }
        if (object_size >= 50_KB)
        {
            return 4;
        }
        if (object_size >= 1_KB)
        {
            return 16;
        }
        // if (object_size <= 529)
        // {
        //     return 64;
        // }
        return 32;
    }
    bool strict_cache_size(size_t size) const override
    {
        if (size <= 529)
        {
            return false;
        }
        return true;
    }

private:
};

}  // namespace avis