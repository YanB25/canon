#pragma once
#include <cinttypes>

#include "avis/config.h"
#include "memory/allocator.h"
namespace mem
{
// This implementation should be fast and generate very few fragmentation.
// NOT THREAD SAFE
class LazySlabAllocator : public BaseAllocator
{
public:
    using Block = std::stack<void *>;

    LazySlabAllocator(BaseAllocator::pointer alloc, size_t block_size)
        : alloc_(alloc), block_size_(block_size)
    {
        CHECK_EQ(block_size_, round_up(block_size_))
            << "** possible dead recursive";
    }
    void free(void *addr) override
    {
        LOG(FATAL) << "** please give me the size. " << addr;
    }
    void free_block(void *addr, size_t size)
    {
        // TODO: could do better
        // If size % block_size_ is not zero, we can also use the remaining
        // memory.
        if (likely(addr != nullptr))
        {
            for (size_t offset = 0; offset < size; offset += block_size_)
            {
                free((char *) addr + offset, block_size_);
            }
            // LOG(INFO) << "[slab] freed block " << PRE(addr, size) << ", waste
            // "
            //           << util::pre_byte(size % block_size_);
        }
    }
    void free(void *addr, size_t size) override
    {
        if (likely(addr != nullptr))
        {
            auto round_size = round_up(size);
            // bypass
            if (unlikely(round_size > block_size_))
            {
                if (likely(alloc_ != nullptr))
                {
                    alloc_->free(addr, size);
                }
                else
                {
                    free_block(addr, size);
                }
            }
            else
            {
                auto &blocks = blocks_[round_size];
                blocks.push(DCHECK_NOTNULL(addr));

                metrics_[round_size].second++;
            }
        }
    }
    void *alloc(size_t size, size_t alignment = 1) override
    {
        // round up
        auto round_size = round_up(size);
        auto &blocks = blocks_[round_size];

        // if larger than block,
        // we bypass any slabs
        if (unlikely(round_size > block_size_))
        {
            if (likely(alloc_ != nullptr))
            {
                auto *ret = alloc_->alloc(size, alignment);
                DCHECK_EQ((uint64_t) ret % alignment, 0);
                return ret;
            }
            return nullptr;
        }

        // allocate from block first
        if (likely(!blocks.empty()))
        {
            auto *ret = DCHECK_NOTNULL(blocks.top());
            blocks.pop();

            metrics_[round_size].first++;
            DCHECK_EQ((uint64_t) ret % alignment, 0);
            return ret;
        }
        else
        {
            if (unlikely(alloc_ == nullptr))
            {
                return nullptr;
            }

            void *new_block = alloc_->alloc(block_size_, alignment);
            if (unlikely(new_block == nullptr))
            {
                return nullptr;
            }

            upstream_alloc_nr_++;
            for (size_t i = 0; i < block_size_ / round_size; ++i)
            {
                void *ith_addr =
                    (void *) ((uint64_t) new_block + i * round_size);
                blocks.push(DCHECK_NOTNULL(ith_addr));
            }

            // try again
            auto *again = alloc(size, alignment);
            DCHECK_EQ((uint64_t) again % alignment, 0);
            return again;
        }
    }
    const auto &metrics() const
    {
        return metrics_;
    }
    auto upstream_alloc_nr() const
    {
        return upstream_alloc_nr_;
    }
    friend std::ostream &operator<<(std::ostream &, const LazySlabAllocator &a);

private:
    __attribute__((always_inline)) size_t round_up(size_t size) const
    {
        size_t round_size = 0;
        if (size <= 8)
        {
            round_size = 8;
        }
        else
        {
            round_size = util::round_up_next_power_of_two(size);
        }
        return round_size;
        // NOTE: so hard to ensure alignment with these slabs

        // if (avis::Config::ins().use_buddy(size))
        // {
        //     return util::round_up_next_power_of_two(size);
        // }
        // return avis::Config::ins().to_class_size(size);
    }
    BaseAllocator::pointer alloc_;
    size_t block_size_;
    // class size => slabs
    std::unordered_map<size_t, Block> blocks_;

    // class size => alloc / dealloc
    std::unordered_map<size_t, std::pair<size_t, size_t>> metrics_;
    size_t upstream_alloc_nr_;
};

inline std::ostream &operator<<(std::ostream &os, const LazySlabAllocator &a)
{
    os << "[Slab] ";
    if (a.blocks_.empty())
    {
        os << "empty" << std::endl;
    }
    else
    {
        os << std::endl;
        for (const auto &[size, blocks] : a.blocks_)
        {
            auto block_nr = blocks.size();
            auto effective_size = block_nr * size;
            os << "size " << util::pre_byte(size) << " has "
               << util::pre_num(block_nr) << " ("
               << util::pre_byte(effective_size) << ")" << std::endl;
        }
    }
    return os;
}

}  // namespace mem