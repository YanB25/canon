#pragma once
#include <cstddef>
#include <memory>

#include "../mm.h"
#include "Metrics.h"

namespace avis::policy
{
class AvisAllocator
{
public:
    using Pointer = std::unique_ptr<AvisAllocator>;
    virtual AvisAddress alloc(size_t size) = 0;
    virtual void free(const AvisAddress &, size_t size) = 0;
    virtual ~AvisAllocator() = default;
    virtual RemoteMetrics remote_metrics() = 0;
};

};  // namespace avis::policy