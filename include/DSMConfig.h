#pragma once
#ifndef __CONFIG_H__
#define __CONFIG_H__

#include "Common.h"
#include "gflags/gflags.h"
#include "util/Literals.h"
#include "util/gflags_dec.h"

using namespace util::literals;

DECLARE_uint64(numa_id);

struct DSMCacheConfig
{
    uint64_t cacheSize{::config::kDefaultCacheSize};
};
struct DSMConfig
{
    DSMCacheConfig cacheConfig;
    uint32_t machineNR{FLAGS_machine_nr};
    uint64_t dsmSize{::config::kDefaultDSMSize};
    size_t dsmReserveSize{0};  // how much size dsm should reserve for its user

    size_t numa_id{FLAGS_numa_id};
    std::string rnic{FLAGS_rnic};

    size_t dir_thread_nr{1};
    size_t worker_nr{1};

    // This directory creates MR with
    // IBV_EXP_ACCESS_RELAXED_ORDERING
    bool relaxed_ordering{false};
};

#endif /* __CONFIG_H__ */
