#pragma once
#ifndef SHERMAM_UTIL_ZIP_RAND_H_
#define SHERMAM_UTIL_ZIP_RAND_H_

#include <glog/logging.h>

#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <memory>

#include "city.h"
#include "util/Rand.h"
#include "util/zipf.h"

namespace util
{
class Generator
{
public:
    virtual ~Generator() = default;
    virtual uint64_t Next() = 0;

private:
};

class ShuffleGenerator : public Generator
{
public:
    ShuffleGenerator(std::unique_ptr<Generator> g, size_t range)
        : g_(std::move(g)), range_(range)
    {
    }
    uint64_t Next() override
    {
        auto next = g_->Next();
        auto ret = CityHash64((const char *) &next, sizeof(next)) % range_;
        return ret;
    }

private:
    std::unique_ptr<Generator> g_;
    size_t range_;
};

class UniformGenerator : public Generator
{
public:
    UniformGenerator(uint64_t min, uint64_t max) : min_(min), max_(max)
    {
    }
    UniformGenerator(uint64_t max) : min_(0), max_(max)
    {
    }
    uint64_t Next() override
    {
        return fast_pseudo_rand_int(min_, max_);
    }

private:
    uint64_t min_;
    uint64_t max_;
};

class BaseZipfianGenerator : public Generator
{
public:
    constexpr static const double kZipfianConst = 0.99;

    BaseZipfianGenerator(uint64_t max,
                         double zipfian_const = kZipfianConst,
                         uint64_t seed = 0)
    {
        mehcached_zipf_init(&state_, max, zipfian_const, seed);
    }

    uint64_t Next() override
    {
        return mehcached_zipf_next(&state_);
    }

private:
    zipf_gen_state state_;
};

class ZipfianGenerator : public Generator
{
public:
    constexpr static const double kZipfianConst = 0.99;
    ZipfianGenerator(uint64_t min,
                     uint64_t max,
                     double zipfian_const = kZipfianConst,
                     uint64_t seed = 0)
        : base_(min), g_(max - min, zipfian_const, seed)
    {
    }
    uint64_t Next() override
    {
        return g_.Next() + base_;
    }

private:
    uint64_t base_;
    BaseZipfianGenerator g_;
};

}  // namespace util

#endif