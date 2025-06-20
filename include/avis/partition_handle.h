#pragma once
#include <fmt/format.h>

#include <cinttypes>

#include "./config.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "GlobalAllocator.h"
#include "Metrics.h"
#include "memory/refill_allocator.h"
#include "memory/rolling_allocator.h"
namespace avis
{
struct PartitionMetric
{
    AllocMetrics am_;
    uint64_t occupied_;
    std::map<size_t, size_t> cached_nr_;
};
inline std::ostream &operator<<(std::ostream &os, const PartitionMetric &m)
{
    os << "{PartitionMetric: occupied: " << m.occupied_ << " ("
       << util::pre_byte(m.occupied_) << ") am_: " << m.am_ << std::endl;
    os << PRE(m.cached_nr_);
    os << std::endl << "}";
    return os;
}
class PartitionHandle
{
public:
    PartitionHandle(DSM::pointer dsm, size_t server_nid, size_t size)
        : dsm_(dsm), server_nid_(server_nid), size_(size)
    {
        gaddr_ = dsm_->alloc_from(size_, server_nid_, 4_KB);
        CHECK(!gaddr_.is_null())
            << "Run ouf of memory when alloc(" << size_ << ")";
    }
    auto block_metrics() const
    {
        return am_;
    }
    PartitionMetric partition_metric() const
    {
        PartitionMetric ret;
        ret.am_ = am_;
        ret.occupied_ = cur_;
        for (const auto &[size, stack] : cached_)
        {
            if (!stack.empty())
            {
                ret.cached_nr_[size] = stack.size();
            }
        }
        return ret;
    }

    GlobalAddress alloc(size_t size)
    {
        auto it = cached_.find(size);
        if (it != cached_.end())
        {
            if (!it->second.empty())
            {
                auto ret = it->second.top();
                it->second.pop();
                am_.record_alloc(size);
                return ret;
            }
        }
        if (cur_ % 8 != 0)
        {
            cur_ += 8 - (cur_ % 8);
        }
        if (unlikely(cur_ + size >= size_))
        {
            return GlobalAddress::Null();
        }
        auto ret = gaddr_ + cur_;
        cur_ += size;
        am_.record_alloc(size);
        return ret;
    }
    void free(GlobalAddress gaddr, size_t size)
    {
        if (!gaddr.is_null())
        {
            am_.record_dealloc(size);
            cached_[size].push(gaddr);
        }
    }

private:
    DSM::pointer dsm_;
    size_t server_nid_;
    size_t size_;
    GlobalAddress gaddr_;
    size_t cur_{0};
    std::map<size_t, std::stack<GlobalAddress>> cached_;

    AllocMetrics am_;
};
}  // namespace avis
