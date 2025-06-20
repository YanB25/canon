#pragma once
#include "GlobalAddress.h"
#include "Metrics.h"

namespace avis
{
class Allocator
{
public:
    virtual GlobalAddress alloc(size_t) = 0;
    virtual void free(GlobalAddress) = 0;
    virtual bool contains(GlobalAddress) = 0;
    virtual void report() const
    {
        LOG(FATAL) << "** Operation not supported.";
    }
    virtual void metric_reset()
    {
    }
    virtual std::pair<AllocMetrics, RemoteMetrics> metrics() const = 0;
    virtual void drain() = 0;
    virtual ~Allocator() = default;
    virtual bool fast_path_available() const = 0;
    virtual GlobalAddress meta_raddr() const = 0;
};
}  // namespace avis