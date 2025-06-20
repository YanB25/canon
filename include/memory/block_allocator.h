#pragma once
#include "memory/allocator.h"
#include "util/Bitset.h"
#include "util/CRTP.h"
#include "util/lock/ReaderPriorityLock.h"

namespace mem
{
// This allocator is thread-safe but may not be very fast
// use it to allocate large chunk
class BlockAllocator : public BaseAllocator
{
public:
    using pointer = std::shared_ptr<BlockAllocator>;
    BlockAllocator(void *buffer, size_t buf_size, size_t block_size)
        : buffer_(buffer),
          size_(buf_size),
          block_size_(block_size),
          bs_(buf_size / block_size)
    {
    }
    size_t block_size() const
    {
        return block_size_;
    }
    void *base_addr() const
    {
        return buffer_;
    }

    void *alloc(size_t size, size_t alignment) override
    {
        auto alloc_blk_nr = round_up_div(size, block_size_);
        auto idx = bs_.atomic_random_set_n(alloc_blk_nr);
        if (idx)
        {
            auto *ret = ith_buf(*idx);
            DCHECK_EQ((uint64_t) ret % alignment, 0)
                << "** block_allocator does not support alignment";
            return ret;
        }
        return nullptr;
    }
    void free(void *) override
    {
        LOG(FATAL) << "** not supported: please give me the size.";
    }
    void free(void *buf, size_t size) override
    {
        auto idx = buf_to_idx(buf);
        auto free_blk_nr = round_up_div(size, block_size_);
        bs_.atomic_unset_n(idx, free_blk_nr);
        return free(buf);
    }

private:
    void *buffer_;
    [[maybe_unused]] size_t size_;
    size_t block_size_;
    util::synchronize::Bitset bs_;

    void *ith_buf(size_t idx) const
    {
        return (void *) ((uint64_t) buffer_ + idx * block_size_);
    }
    size_t buf_to_idx(void *buf) const
    {
        off_t off = (uint64_t) buf - (uint64_t) buffer_;
        DCHECK_EQ(off % block_size_, 0);
        return off / block_size_;
    }
};

}  // namespace mem