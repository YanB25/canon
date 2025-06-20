#pragma once
#include <chrono>
#include <utility>

#include "GlobalAddress.h"
#include "allocator.h"
#include "util/PerformanceReporter.h"
#include "util/TimeConv.h"
using namespace std::chrono_literals;

class DSM;

namespace memory
{
class RMChunkAllocator : public mem::BaseAllocator
{
public:
    RMChunkAllocator(DSM *dsm, size_t node_id, size_t dir_id);
    void *alloc(size_t size, size_t alignment) override
    {
        auto *ret = do_alloc(size, alignment);
        // We give you another chance
        if (unlikely(GlobalAddress(ret).is_null()))
        {
            ret = do_alloc(size, alignment);
        }
        return ret;
    }
    void *do_alloc(size_t size, size_t alignment);
    void free(void *addr, size_t size) override;
    void free(void *) override
    {
        LOG(FATAL) << "** Unsupported operation";
    }
    constexpr size_t node_id() const
    {
        return node_id_;
    }
    constexpr size_t dir_id() const
    {
        return dir_id_;
    }
    static auto &metrics()
    {
        auto min = util::time::to_ns(0ns);
        auto max = util::time::to_ns(100us);
        auto step = util::time::to_ns(100ns);
        static OnePassBucketMonitor<uint64_t> bucket(min, max, step);
        return bucket;
    }

private:
    DSM *dsm_;
    size_t node_id_;
    size_t dir_id_;
    std::stack<void *> addrs_;
};

class RRAllocator : public mem::BaseAllocator
{
public:
    using pointer = std::shared_ptr<RRAllocator>;
    RRAllocator(const std::vector<mem::BaseAllocator::pointer> &allocators,
                size_t start_from)
        : allocators_(allocators), rr_idx_(start_from % allocators.size())
    {
    }
    void *alloc_from(size_t size, size_t idx, size_t alignment)
    {
        auto &alloc = allocators_[idx % allocators_.size()];
        return alloc->alloc(size, alignment);
    }
    void free_to(void *addr, size_t size, size_t idx)
    {
        auto &alloc = allocators_[idx % allocators_.size()];
        alloc->free(addr, size);
    }
    void *alloc(size_t size, size_t alignment) override
    {
        void *ret;
        size_t try_nr = 0;
        size_t limit = allocators_.size();
        while (try_nr++ < limit)
        {
            auto &alloc = allocators_[rr_idx_];
            ret = alloc->alloc(size, alignment);
            // LOG(INFO) << "[RR] allocating from " << PRE(rr_idx_) << " got "
            //           << (void *) ret;
            rr_idx_++;
            if (rr_idx_ >= limit)
            {
                rr_idx_ = 0;
            }
            if (likely(ret != nullptr))
            {
                return ret;
            }
        }
        LOG(WARNING) << "RRAllocator exhausted";
        return ret;
    }
    void free(void *addr, size_t size) override
    {
        if (addr)
        {
            auto &alloc = allocators_[rr_idx_];
            alloc->free(addr, size);
            rr_idx_++;
            if (rr_idx_ >= allocators_.size())
            {
                rr_idx_ = 0;
            }
        }
    }
    void free(void *) override
    {
        LOG(FATAL) << "** Unsupported operation";
    }
    size_t allocator_size() const
    {
        return allocators_.size();
    }

private:
    std::vector<mem::BaseAllocator::pointer> allocators_;
    size_t rr_idx_;
};
}  // namespace memory