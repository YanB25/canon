#include "memory/dsm_allocator_impl.h"

#include <limits>

#include "Common.h"
#include "DSM.h"
#include "GlobalAddress.h"

namespace memory
{
RMChunkAllocator::RMChunkAllocator(DSM *dsm, size_t node_id, size_t dir_id)
    : dsm_(dsm), node_id_(node_id), dir_id_(dir_id)
{
}
void *RMChunkAllocator::do_alloc(size_t size, size_t alignment)
{
    // If post alignment requirement
    // We may not be able to re-use the freed memory
    if (!addrs_.empty() && alignment <= 8)
    {
        void *ret = (void *) addrs_.top();
        addrs_.pop();
        DCHECK_EQ((uint64_t) ret % alignment, 0)
            << "** alignment violation: " << PRE(ret, alignment);
        return DCHECK_NOTNULL(ret);
    }

    RawMessage m;
    m.type = RpcType::MALLOC;
    m.alloc_size = size;
    CHECK_LT(size, std::numeric_limits<decltype(m.alloc_size)>::max());
    m.alloc_la = alignment <= 8 ? 0 : log2(alignment);

    DCHECK_GE(size, define::kChunkSize)
        << "** refill " << PRE(size) << " smaller than " << define::kChunkSize;

    // ChronoTimer timer;
    dsm_->rpc_call_dir(m, node_id_, dir_id_);
    GlobalAddress gaddr = dsm_->rpc_wait()->addr;
    auto offset = gaddr.offset;
    // auto ns = timer.pin();
    // metrics().collect(ns);
    // LOG_EVERY_N(WARNING, (int) 1_K) << "Allocation: " << metrics();
    return (void *) offset;
}

void RMChunkAllocator::free(void *addr, size_t size)
{
    if constexpr (debug())
    {
        CHECK_EQ(GlobalAddress(addr).nodeID, 0)
            << "** node_id should be wiped out here";
    }

    if (addr)
    {
        CHECK_EQ(size % define::kChunkSize, 0);
        for (size_t i = 0; i < size / define::kChunkSize; ++i)
        {
            char *cur_addr = (char *) addr + i * define::kChunkSize;
            addrs_.push(cur_addr);
        }
    }
}

}  // namespace memory