#pragma once

#include <cinttypes>
#include <functional>
#include <iostream>

#include "./policy.h"
#include "GlobalAddress.h"
#include "glog/logging.h"
#include "util/CRTP.h"

namespace avis
{
class AvisAddress
{
public:
    union
    {
        struct
        {
            uint32_t offset_;
            uint16_t region_id_;
            uint16_t node_id_;
        };
        uint64_t val_;
    };
    bool operator==(const AvisAddress &rhs) const
    {
        return val_ == rhs.val_;
    }

    operator bool() const
    {
        return !is_null();
    }

    AvisAddress() : val_(0)
    {
    }
    AvisAddress(uint16_t node_id, uint16_t region_id, uint32_t offset)
        : offset_(offset), region_id_(region_id), node_id_(node_id)
    {
        DCHECK_LE((uint64_t) offset,
                  std::numeric_limits<decltype(offset_)>::max());
        DCHECK_LE(node_id, std::numeric_limits<uint8_t>::max())
            << "Some applications, e.g., RACE hashing may require <= 16 bits "
               "total meta";
        DCHECK_LE(region_id, std::numeric_limits<uint8_t>::max());
    }
    auto node_id() const
    {
        return node_id_;
    }
    auto region_id() const
    {
        return region_id_;
    }
    auto offset() const
    {
        return offset_;
    }
    bool is_null() const
    {
        return offset_ == 0;
    }
    uint64_t val() const
    {
        return val_;
    }

    // [node_id, region_id, offset]
    static AvisAddress from_raddr(GlobalAddress raddr);
};

static_assert(sizeof(AvisAddress) == sizeof(uint64_t));
inline std::ostream &operator<<(std::ostream &os, const AvisAddress &a)
{
    os << "{AvisAddress node_id: " << a.node_id()
       << ", region_id: " << a.region_id()
       << ", offset: " << (void *) (uint64_t) a.offset() << "}";
    return os;
}

}  // namespace avis

namespace std
{
template <>
struct hash<avis::AvisAddress>
{
    size_t operator()(const avis::AvisAddress &x) const
    {
        return hash<uint64_t>()(x.val());
    }
};
}  // namespace std