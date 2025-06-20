#pragma once
#ifndef SHERMEM_CORO_CONTEXT_H_
#define SHERMEM_CORO_CONTEXT_H_

#include <iostream>

#include "Common.h"
#include "WRID.h"
#include "patronus/BasicType.h"
#include "util/Coro.h"

class CoroContext;

class CoroControlBlock
{
public:
    using pointer = std::unique_ptr<CoroControlBlock>;
    using ReasonPrefix = uint64_t;
    using Reason = std::pair<ReasonPrefix, uint64_t>;

    static pointer make_ptr()
    {
        return std::make_unique<CoroControlBlock>();
    }

    void wait(CoroContext *ctx);
    bool is_wait_condition(coro_t) const;
    std::optional<coro_t> next_waiting_coro();

    friend std::ostream &operator<<(std::ostream &os,
                                    const CoroControlBlock &cb);

private:
    std::list<size_t> wait_coros_{};
    std::array<std::atomic<bool> *, define::kMaxCoroNr> wait_condition_{};
};

inline std::ostream &operator<<(std::ostream &os, const CoroControlBlock &cb)
{
    os << "{CB wait_coro: " << util::pre(cb.wait_coros_)
       << ", condition: " << util::pre(cb.wait_condition_) << "}";
    os << std::endl;
    return os;
}

enum class CoroWaitReason
{
    kRPC,
    kRDMA,
    kRunning,
    kExited,
    kIdle,
};
inline std::ostream &operator<<(std::ostream &os, CoroWaitReason r)
{
    switch (r)
    {
    case CoroWaitReason::kRPC:
        os << "RPC";
        break;
    case CoroWaitReason::kRDMA:
        os << "RDMA";
        break;
    case CoroWaitReason::kRunning:
        os << "Running";
        break;
    case CoroWaitReason::kExited:
        os << "Exited";
        break;
    case CoroWaitReason::kIdle:
        os << "Idle";
        break;
    default:
        os << "Unknown(" << (int) r << ")";
        break;
    }
    return os;
}
struct CoroState
{
    size_t tid{0};
    size_t coro_id{kNotACoro};
    enum CoroWaitReason wait_reason;

    // for kRPC
    patronus::RpcType expect_resp_type;

    // for RDMA
    uint64_t expect_wr_id;
    void record_yield_reason(patronus::AsyncContext<patronus::RPC> *rpc_context,
                             patronus::BaseMessage *msg);
    void record_yield_reason(void *wr_ctx)
    {
        DCHECK_EQ(wait_reason, CoroWaitReason::kRunning);
        wait_reason = CoroWaitReason::kRDMA;
        expect_wr_id = (uint64_t) wr_ctx;
    }
    void match_yield_reason(void *wr_ctx)
    {
        DCHECK_EQ(wait_reason, CoroWaitReason::kRDMA);
        DCHECK_EQ(expect_wr_id, (uint64_t) wr_ctx);
        wait_reason = CoroWaitReason::kRunning;
    }
    void match_yield_reason(patronus::AsyncContext<patronus::RPC> *rpc_context,
                            patronus::BaseMessage *msg);
    void record_yield_reason(patronus::AsyncContext<patronus::RDMA> *rw_context,
                             const patronus::WRID &wrid);
    void match_yield_reason(patronus::AsyncContext<patronus::RDMA> *rw_context,
                            const patronus::WRID &wrid);
    void record_yield_reason(bool exit);
    ~CoroState()
    {
    }
};
std::ostream &operator<<(std::ostream &os, const CoroState &s);

namespace patronus
{
struct RPC;
}
class CoroContext
{
public:
    CoroContext(size_t thread_id,
                CoroYield *yield,
                CoroCall *master,
                coro_t coro_id,
                CoroControlBlock *cb = nullptr);
    CoroContext(size_t thread_id,
                CoroYield *yield,
                CoroCall *workers,
                CoroControlBlock *cb = nullptr);
    CoroContext(const CoroContext &) = delete;
    CoroContext &operator=(const CoroContext &) = delete;

    CoroContext();
    bool is_master() const;
    bool is_worker() const;
    bool is_nullctx() const;
    coro_t coro_id() const;
    size_t thread_id() const;

    std::atomic<bool> *wait_variable()
    {
        DCHECK(is_worker());
        return &wait_condition_;
    }

    void record_yield_reason(patronus::AsyncContext<patronus::RPC> *rpc_context,
                             patronus::BaseMessage *msg);
    void record_yield_reason(patronus::AsyncContext<patronus::RDMA> *rw_context,
                             const patronus::WRID &wrid);
    void record_yield_reason(bool exit);
    void record_yield_reason(void *wr_id);

    void match_yield_reason(void *wr_id);

    void yield_to_master();
    void yield_to_worker(coro_t wid);
    friend std::ostream &operator<<(std::ostream &os, const CoroContext &ctx);

    // The below three APIs are extension to CoroContext,
    // relying on a centralized CoroControlBlock
    // It behaves like a conditional variable {reason_prefix, reason_data},
    // that allows accurate scheduling of worker coroutines.
    using ReasonPrefix = CoroControlBlock::ReasonPrefix;
    void wait_yield_to_master();
    std::optional<coro_t> next_hot_waiting_coro();
    bool is_hot_waiting_coro(coro_t) const;

    void set_trace(trace_t trace);
    trace_t trace() const;
    ContTimer<::config::kEnableTrace> &timer();
    CoroState *coro_state();
    const CoroState *coro_state() const;
    ~CoroContext();

    auto *cb()
    {
        return cb_;
    }

private:
    CoroYield *yield_{nullptr};
    CoroCall *master_{nullptr};
    CoroCall *workers_{nullptr};

    CoroState state_;
    // size_t thread_id_{0};
    // coro_t coro_id_{kNotACoro};
    CoroControlBlock *cb_{nullptr};

    trace_t trace_{0};
    ContTimer<::config::kEnableTrace> timer_;

    std::atomic<bool> wait_condition_{false};  // wait until condition is true
};

static CoroContext nullctx;

inline std::ostream &operator<<(std::ostream &os, const CoroContext &ctx)
{
    if (ctx.coro_id() == kMasterCoro)
    {
        os << "{Coro T(" << ctx.thread_id() << ") Master }";
    }
    else if (ctx.coro_id() == kNotACoro)
    {
        os << "{Coro Not a coro}";
    }
    else
    {
        os << "{Coro T(" << ctx.thread_id() << ") " << (int) ctx.coro_id()
           << "}";
    }
    return os;
}

struct Void
{
};

template <size_t kCoroCnt_, typename T>
class CoroExecutionContextWith
{
public:
    constexpr static size_t kCoroCnt = kCoroCnt_;

    void worker_finished(size_t coro_id)
    {
        finish_all_[coro_id] = true;
    }
    bool is_finished_all() const
    {
        return std::all_of(
            finish_all_.begin(), finish_all_.end(), [](bool i) { return i; });
    }
    bool is_finished(size_t wid) const
    {
        DCHECK_LT(wid, kCoroCnt);
        return finish_all_[wid];
    }
    CoroCall *workers()
    {
        return workers_.data();
    }
    const CoroCall *workers() const
    {
        return workers_.data();
    }
    CoroCall &worker(size_t wid)
    {
        CHECK_LT(wid, kCoroCnt);
        return workers_[wid];
    }
    const CoroCall &master() const
    {
        return master_;
    }
    CoroCall &master()
    {
        return master_;
    }
    T &get_private_data()
    {
        return t_;
    }
    const T &get_private_data() const
    {
        return t_;
    }

private:
    std::array<CoroCall, kCoroCnt> workers_{};
    CoroCall master_;
    std::array<bool, kCoroCnt> finish_all_{};
    T t_;
};
template <size_t size>
using CoroExecutionContext = CoroExecutionContextWith<size, Void>;

class pre_coro_ctx
{
public:
    pre_coro_ctx(CoroContext *ctx) : ctx_(ctx)
    {
    }
    CoroContext *ctx_;
};
inline std::ostream &operator<<(std::ostream &os, const pre_coro_ctx &pctx)
{
    if (pctx.ctx_)
    {
        os << *pctx.ctx_;
    }
    else
    {
        os << "{no coro ctx}";
    }
    return os;
}

#endif