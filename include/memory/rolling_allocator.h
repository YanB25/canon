#pragma once
#include <atomic>

#include "Metrics.h"
#include "memory/allocator.h"
#include "util/Likely.h"
#include "util/Util.h"
#include "util/lock/Guard.h"
#include "util/lock/RWLock.h"
namespace mem
{
class RollingAllocator : public ::mem::IAllocator, public ::mem::BaseAllocator
{
public:
    using pointer = std::shared_ptr<RollingAllocator>;
    RollingAllocator(void *buf, size_t size)
        : buf_((char *) buf), offset_(0), size_(size)
    {
    }
    static std::shared_ptr<RollingAllocator> new_instance(void *buf,
                                                          size_t size)
    {
        return std::make_shared<RollingAllocator>(buf, size);
    }
    void *alloc(size_t size, size_t alignment) override
    {
        auto *ret = do_alloc_align(size, alignment);
        if (ret != nullptr)
        {
            am_.record_alloc(size);
        }
        return ret;
    }
    size_t alloc_nr() const
    {
        return allocated_nr_;
    }
    void *alloc(size_t size, CoroContext *) override
    {
        return do_alloc_align(size, 1 /* alignment */);
    }
    void free(void *, CoroContext *) override
    {
    }
    void free(void *) override
    {
    }
    void free(void *, size_t, CoroContext *) override
    {
    }
    void free(void *, size_t) override
    {
    }

    auto metrics() const
    {
        return am_;
    }

private:
    char *buf_;
    uint64_t offset_{};
    size_t size_;
    std::atomic<size_t> allocated_nr_{};
    util::RWLock lock_;

    AllocMetrics am_;

    void *do_alloc_align(size_t size, size_t alignment)
    {
        // We use lock, so that we will not waste memory
        // into satisfying alignment (which is hard to be atomic)
        util::UniqueGuard<util::RWLock> guard(lock_);

        auto padding = ((uint64_t) buf_ + offset_) % alignment;
        if (unlikely(padding != 0))
        {
            offset_ += alignment - padding;
        }

        DCHECK_EQ((uint64_t) (buf_ + offset_) % alignment, 0);
        if (unlikely(offset_ + size > size_))
        {
            // run out of memory
            return nullptr;
        }
        allocated_nr_.fetch_add(1, std::memory_order_relaxed);
        uint64_t ret = offset_;
        offset_ += size;
        return (void *) (buf_ + ret);
    }
};

}  // namespace mem