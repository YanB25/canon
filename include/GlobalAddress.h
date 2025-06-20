#pragma once
#ifndef __GLOBALADDRESS_H__
#define __GLOBALADDRESS_H__

#include "util/Debug.h"

class GlobalAddress;
class GlobalAddress
{
public:
    union
    {
        struct
        {
            uint64_t offset : 48;
            uint64_t nodeID : 16;
        };
        uint64_t val;
    };

    explicit GlobalAddress() : val(0)
    {
    }
    explicit GlobalAddress(void *addr) : val((uint64_t) addr)
    {
    }
    explicit GlobalAddress(uint16_t node_id, uint64_t offset)
        : offset(offset), nodeID(node_id)
    {
    }
    bool is_null() const
    {
        return offset == 0;
    }

    static GlobalAddress Null()
    {
        static GlobalAddress zero;
        return zero;
    };
    auto operator<=>(const GlobalAddress &rhs) const
    {
        DCHECK_EQ(nodeID, rhs.nodeID);
        return val <=> rhs.val;
    }
    bool operator==(const GlobalAddress &rhs) const
    {
        return val == rhs.val;
    }
    GlobalAddress operator+(ssize_t offset) const
    {
        if constexpr (debug())
        {
            auto old_val = val;
            auto new_val = val + offset;
            auto old_gaddr = GlobalAddress((void *) old_val);
            auto new_gaddr = GlobalAddress((void *) new_val);
            CHECK_EQ(old_gaddr.nodeID, new_gaddr.nodeID)
                << "** gaddr overflow detected. old_gaddr.offset "
                << old_gaddr.offset << ", new_gaddr.offset "
                << new_gaddr.offset;
        }
        GlobalAddress ret = *this;
        ret.offset += offset;
        return ret;
    }
    GlobalAddress operator-(ssize_t off) const
    {
        GlobalAddress ret = *this;
        DCHECK_GE(ret.offset, off) << "** gaddr overflow detected";
        ret.offset -= off;
        return ret;
    }
    static GlobalAddress from_raddr(GlobalAddress);
} __attribute__((packed));

static_assert(sizeof(GlobalAddress) == sizeof(uint64_t), "XXX");

static inline std::ostream &operator<<(std::ostream &os,
                                       const GlobalAddress &addr)
{
    os << "{gaddr " << addr.nodeID << ", " << (void *) addr.offset << "}";
    return os;
}

inline GlobalAddress GADD(const GlobalAddress &addr, int off)
{
    auto ret = addr;
    ret.offset += off;
    return ret;
}

static GlobalAddress nullgaddr;

namespace std
{
template <>
struct hash<GlobalAddress>
{
    size_t operator()(const GlobalAddress &gaddr) const
    {
        return std::hash<uint64_t>()(gaddr.val);
    }
};
}  // namespace std

#endif /* __GLOBALADDRESS_H__ */
