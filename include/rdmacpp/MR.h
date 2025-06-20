#pragma once
#include <cstdint>
#include <iostream>

#include "infiniband/verbs.h"
#include "util/Pre.h"

namespace rdma
{
struct MemoryRegion
{
    void *addr;
    size_t length;
    uint32_t lkey;
    uint32_t rkey;
    ibv_mr *mr;
};

inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::rdma::MemoryRegion &v)
{
    os << "{MemoryRegion ";
    os << "addr: " << util::pre(v.addr);
    os << ", length: " << util::pre(v.length);
    os << ", lkey: " << util::pre(v.lkey);
    os << ", rkey: " << util::pre(v.rkey);
    os << ", mr: " << util::pre(v.mr);
    os << "}";
    return os;
}

}  // namespace rdma