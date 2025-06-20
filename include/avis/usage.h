#pragma once

#include "DSM.h"
#include "PerThread.h"

namespace avis
{
class Usage
{
public:
    static Usage &ins()
    {
        static Usage ins_;
        return ins_;
    }
    void collect(size_t size)
    {
        usage_.current() += size;
    }
    uint64_t sum(DSM::pointer dsm)
    {
        uint64_t local_sum = usage_.sum();
        return *dsm->sum(local_sum, 100ms);
    }

private:
    Usage() = default;
    Perthread<uint64_t> usage_;
};
};  // namespace avis