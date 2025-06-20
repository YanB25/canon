#pragma once
#ifndef MEMORY_ALLOCATOR_H_
#define MEMORY_ALLOCATOR_H_

#include "Common.h"
#include "CoroContext.h"

using namespace util::literals;
namespace mem
{
inline std::pair<void *, size_t> align_address(void *addr,
                                               size_t size,
                                               size_t align)
{
    auto diff = ((uint64_t) addr) % align;
    if (diff == 0)
    {
        return {addr, size};
    }
    auto adjust = align - diff;
    auto aligned_addr = (void *) ((uint64_t) addr + adjust);
    if (size < adjust)
    {
        return {nullptr, 0};
    }
    auto aligned_size = size - adjust;
    return {aligned_addr, aligned_size};
}

class IAllocator
{
public:
    using pointer = std::shared_ptr<IAllocator>;
    virtual ~IAllocator() = default;
    virtual void *alloc(size_t size, CoroContext *ctx = nullptr) = 0;
    virtual void free(void *addr, size_t size, CoroContext *ctx = nullptr) = 0;
    virtual void free(void *addr, CoroContext *ctx = nullptr) = 0;
};

class BaseAllocator
{
public:
    using pointer = std::shared_ptr<BaseAllocator>;
    virtual ~BaseAllocator() = default;
    virtual void *alloc(size_t size, size_t alignment = 1) = 0;
    virtual void free(void *addr) = 0;
    virtual void free(void *addr, size_t size) = 0;
};
}  // namespace mem

#endif