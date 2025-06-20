#pragma once
#include <limits>

#include "DSM.h"
#include "GlobalAddress.h"
#include "GlobalAllocator.h"
#include "dsm_allocator_impl.h"
#include "tag_allocator.h"
#include "util/CRTP.h"

namespace memory
{
class DSMBaseAllocator
{
public:
    using pointer = std::shared_ptr<DSMBaseAllocator>;
    virtual ~DSMBaseAllocator() = default;
    virtual GlobalAddress alloc(size_t) = 0;
    virtual void free(GlobalAddress addr, size_t size) = 0;
};

struct RemoteTag : public util::Hashable<RemoteTag>,
                   public util::Equable<RemoteTag>
{
    uint8_t node_id;
    uint8_t dir_id;
};

inline std::ostream &operator<<(std::ostream &os, const RemoteTag &t)
{
    os << "{RemoteTag: node_id: " << (int) t.node_id
       << ", dir_id: " << (int) t.dir_id << "}";
    return os;
}

/**
 * The uniform API for dsm->alloc()
 * It
 * - dispatches the memory across all the machines and directory
 * - enable slabs and memory reuse by free
 *
 */
class DSMAllocator
{
public:
    using pointer = std::shared_ptr<DSMAllocator>;
    using node_id_t = uint16_t;
    DSMAllocator(DSM *dsm);
    GlobalAddress alloc_from(size_t size, uint16_t node_id, size_t alignment)
    {
        DCHECK_LE(node_id, std::numeric_limits<uint8_t>::max());
        memory::RemoteTag tag{.node_id = (uint8_t) node_id, .dir_id = 0};
        void *ret = alloc_->alloc_from(size, tag, alignment);
        GlobalAddress gaddr(ret);
        DCHECK_EQ(gaddr.nodeID, 0);
        gaddr.nodeID = node_id;
        return gaddr;
    }
    void free(GlobalAddress gaddr, size_t size)
    {
        DCHECK_LE(gaddr.nodeID, std::numeric_limits<uint8_t>::max());
        memory::RemoteTag tag{.node_id = (uint8_t) gaddr.nodeID, .dir_id = 0};
        gaddr.nodeID = 0;
        alloc_->free_to((void *) gaddr.val, size, tag);
    }
    GlobalAddress alloc(size_t size, size_t alignment = 1)
    {
        auto [tag, addr] = alloc_->alloc(size, alignment);
        GlobalAddress gaddr((void *) addr);
        DCHECK_EQ(gaddr.nodeID, 0);
        gaddr.nodeID = tag.node_id;
        return gaddr;
    }
    friend std::ostream &operator<<(std::ostream &os, const DSMAllocator &a);

private:
    mem::TagAllocator<RemoteTag>::pointer alloc_;
};

inline std::ostream &operator<<(std::ostream &os, const DSMAllocator &a)
{
    os << "[DSM] " << std::endl;
    if (likely(a.alloc_ != nullptr))
    {
        os << *a.alloc_;
    }
    os << "END [DSM]";
    return os;
}

}  // namespace memory