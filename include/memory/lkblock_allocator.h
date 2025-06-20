#pragma once
#include "glog/logging.h"
#include "memory/allocator.h"
#include "util/Pre.h"
#include "util/lock/Guard.h"
#include "util/lock/RWLock.h"

namespace mem
{
class LkBlockAllocator : public BaseAllocator
{
public:
    LkBlockAllocator(void *buffer, size_t buf_size, size_t block_size)
        : buffer_(buffer), size_(buf_size), block_size_(block_size)
    {
        auto block_nr = size_ / block_size_;
        bs_.resize(block_nr, false);
    }

    void *alloc(size_t size) override
    {
        util::UniqueGuard guard(lock_);
        size_t alloc_blk_nr = round_up_div(size, block_size_);

        auto block_nr = bs_.size();
        size_t attempt = 0;
        while (true)
        {
            if (likely(poll_n(idx_, alloc_blk_nr)))
            {
                // found
                set_n(idx_, alloc_blk_nr);
                void *ret = buffer_ + idx_ * block_size_;
                // wrap around
                idx_ = (idx_ + alloc_blk_nr) % block_nr;
                return ret;
            }
            if (unlikely(attempt++ > block_nr))
            {
                return nullptr;
            }
        }
    }

    void free(void *buf) override
    {
        LOG(FATAL) << "** not supported " << PRE(buf);
    }

    void free(void *buf, size_t size) override
    {
        util::UniqueGuard guard(lock_);
        size_t alloc_blk_nr = round_up_div(size, block_size_);
    }

private:
    void *buffer_;
    size_t size_;
    size_t block_size_;

    std::vector<bool> bs_;
    size_t idx_{0};

    util::RWLock lock_;

    bool poll_n(size_t idx, size_t n) const
    {
        for (size_t i = 0; i < n; ++i)
        {
            auto pos = (idx + i) % block_size_;
            if (bs_[pos])
            {
                return false;
            }
        }
        return true;
    }
    void set_n(size_t idx, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
        {
            auto pos = (idx + i) % block_size_;
            DCHECK(!bs_[pos]) << "allocated: " << PRE(idx) << ", " << PRE(i)
                              << ", " << PRE(pos);
            bs_[pos] = true;
        }
    }
};
}  // namespace mem