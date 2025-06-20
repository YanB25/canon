#pragma once
#ifndef SHERMEM_WRID_H_
#define SHERMEM_WRID_H_

#include <infiniband/verbs.h>

#include <cinttypes>
#include <iostream>
#include <limits>

#include "patronus/BasicType.h"
#include "util/Coro.h"
#include "util/TagPtr.h"
#include "util/Tracer.h"

#define WRID_INVALID 1
#define WRID_WAIT_RPC 2
#define WRID_WAIT_RDMA 3
#define WRID_WAIT_MW 4
#define WRID_WAIT_OFFLOAD_MEMCPY 5
#define WRID_PREFIX_UMSG_RECV 6
#define WRID_PREFIX_UMSG_SEND 7
#define WRID_PREFIX_BENCHMARK_ONLY 8

struct pre_wrid_prefix
{
    pre_wrid_prefix(uint64_t p) : prefix(p)
    {
    }
    uint64_t prefix;
};

inline std::ostream &operator<<(std::ostream &os, pre_wrid_prefix p)
{
    switch (p.prefix)
    {
    case WRID_INVALID:
        os << "INVALID";
        return os;
    case WRID_WAIT_RPC:
        os << "WAIT_RPC";
        return os;
    case WRID_WAIT_RDMA:
        os << "WAIT_RDMA";
        return os;
    case WRID_WAIT_MW:
        os << "WAIT_MW";
        return os;
    case WRID_WAIT_OFFLOAD_MEMCPY:
        os << "WAIT_OFFLOAD_MEMCPY";
        return os;
    case WRID_PREFIX_UMSG_RECV:
        os << "PREFIX_UMSG_RECV";
        return os;
    default:
        os << "UNKNOWN";
        LOG(FATAL) << "** unknown WRID prefix: " << p.prefix;
        return os;
    }
    return os;
}

enum class WRContextType
{
    kRPC,
};

template <typename T>
struct AsyncContext
{
    size_t tid;
    size_t coro_id;
    std::atomic<bool> ready;

    T inner;
    CoroState *coro_state;
};

struct RDMA
{
    ibv_wc_status wc_status{IBV_WC_SUCCESS};
    coro_t coro_id;
    size_t target_node;
    size_t dir_id;
    util::TraceView trace_view{util::nulltrace};
};

using WRID = util::TaggedPtrImpl<AsyncContext<RDMA>>;

static WRID nullwrid{nullptr, (uint8_t) WRID_INVALID, (uint8_t) false};

inline std::ostream &operator<<(std::ostream &os, WRID wr_id)
{
    os << "{WRID " << pre_wrid_prefix(wr_id.u8_h())
       << ", signal: " << (bool) wr_id.u8_l()
       << ", object: " << (void *) wr_id.ptr() << "}";
    return os;
}

#endif