#pragma once
#include "rdmacpp/MR.h"
#include "util/CRTP.h"

namespace mem
{
class SecondaryAllocator : public util::MakeShared<SecondaryAllocator>
{
public:
    virtual rdma::MemoryRegion alloc(size_t size) = 0;
    virtual void free(const rdma::MemoryRegion &) = 0;
    virtual ~SecondaryAllocator() = default;
};
}  // namespace mem