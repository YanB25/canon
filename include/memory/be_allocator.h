#pragma once
#include <iostream>
#include <list>
#include <optional>
#include <vector>

#include "Common.h"
#include "GlobalAddress.h"
#include "RawMessageConnection.h"
#include "util/Likely.h"

namespace memory
{
struct RemoteMemoryChunk
{
    RemoteMemoryChunk(GlobalAddress gaddr, size_t remain_size)
        : gaddr(gaddr), remain_size(remain_size)
    {
    }
    GlobalAddress gaddr;
    size_t remain_size;
};
class BestEffortAllocator
{
public:
    BestEffortAllocator()
    {
    }

    void fill(GlobalAddress gaddr, size_t size)
    {
        mem_.emplace_back(gaddr, size);
        fill_nr_++;
    }

    std::optional<GlobalAddress> alloc(size_t size)
    {
        while (likely(!mem_.empty()))
        {
            auto &first = mem_.front();
            if (likely(first.remain_size >= size))
            {
                auto ret = first.gaddr;
                first.remain_size -= size;
                // gaddr is the CURRENT head
                // so advance too
                first.gaddr = first.gaddr + size;

                alloc_nr_++;
                return ret;
            }
            else
            {
                // drop this chunk entirely
                wasted_ += first.remain_size;
                mem_.pop_front();
            }
        }
        DCHECK(mem_.empty());
        return std::nullopt;
    }
    friend std::ostream &operator<<(std::ostream &os,
                                    const BestEffortAllocator &be);

private:
    std::list<RemoteMemoryChunk> mem_;
    size_t wasted_{0};

    size_t fill_nr_{0};
    size_t alloc_nr_{0};
};

inline std::ostream &operator<<(std::ostream &os, const BestEffortAllocator &be)
{
    os << "{BE fill: " << be.fill_nr_ << ", alloc: " << be.alloc_nr_
       << ", wated(B): " << util::pre_byte(be.wasted_) << "}";
    return os;
}
}  // namespace memory