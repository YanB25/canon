#include "CoroContext.h"
CoroContext::CoroContext(size_t thread_id,
                         CoroYield *yield,
                         CoroCall *master,
                         coro_t coro_id,
                         CoroControlBlock *cb)
    : yield_(yield), master_(master), cb_(cb)
{
    CHECK_NE(coro_id, kMasterCoro) << "** This coro should not be a master";
    CHECK_NE(coro_id, kNotACoro) << "** This coro should not be nullctx";
    state_.tid = thread_id;
    state_.coro_id = coro_id;
    state_.wait_reason = CoroWaitReason::kRunning;
}
CoroContext::CoroContext(size_t thread_id,
                         CoroYield *yield,
                         CoroCall *workers,
                         CoroControlBlock *cb)
    : yield_(yield), workers_(workers), cb_(cb)
{
    state_.tid = thread_id;
    state_.coro_id = kMasterCoro;
}

CoroContext::CoroContext()
{
    state_.tid = 0;
    state_.coro_id = kNotACoro;
}
bool CoroContext::is_master() const
{
    return coro_id() == kMasterCoro;
}
bool CoroContext::is_worker() const
{
    return coro_id() != kMasterCoro && coro_id() != kNotACoro;
}
bool CoroContext::is_nullctx() const
{
    return coro_id() == kNotACoro;
}
coro_t CoroContext::coro_id() const
{
    return state_.coro_id;
}
size_t CoroContext::thread_id() const
{
    return state_.tid;
}
void CoroContext::record_yield_reason(bool exit)
{
    state_.record_yield_reason(exit);
}
void CoroState::record_yield_reason(bool exit)
{
    DCHECK_EQ(wait_reason, CoroWaitReason::kRunning);
    if (exit)
    {
        wait_reason = CoroWaitReason::kExited;
    }
    else
    {
        wait_reason = CoroWaitReason::kIdle;
    }
}
void CoroContext::record_yield_reason(
    patronus::AsyncContext<patronus::RDMA> *rw_context,
    const patronus::WRID &wrid)
{
    DCHECK_EQ(rw_context->coro_id, coro_id());
    DCHECK(!rw_context->ready);
    DCHECK_EQ(rw_context->tid, thread_id());
    DCHECK(rw_context->coro_state == nullptr ||
           rw_context->coro_state == coro_state())
        << "** overwrite rw_context->coro_state: " << rw_context->coro_state;
    rw_context->coro_state = coro_state();
    state_.record_yield_reason(rw_context, wrid);
}

void CoroContext::record_yield_reason(void *wr_id)
{
    state_.record_yield_reason(wr_id);
}

void CoroState::record_yield_reason(
    patronus::AsyncContext<patronus::RDMA> *rw_context,
    const patronus::WRID &wrid)
{
    DCHECK_EQ(rw_context->coro_id, coro_id);
    DCHECK_EQ(rw_context->coro_state, this);
    DCHECK_EQ(wait_reason, CoroWaitReason::kRunning);
    wait_reason = CoroWaitReason::kRDMA;
    expect_wr_id = wrid.val();
}
void CoroState::match_yield_reason(
    patronus::AsyncContext<patronus::RDMA> *rw_context,
    const patronus::WRID &wrid)
{
    DCHECK_EQ(rw_context->coro_id, coro_id);
    DCHECK_EQ(rw_context->coro_state, this)
        << "** rw_context->coro_state mismatch detected "
        << rw_context->coro_state;
    DCHECK_EQ(wait_reason, CoroWaitReason::kRDMA);
    DCHECK_EQ(expect_wr_id, wrid.val());
    wait_reason = CoroWaitReason::kRunning;
}

void CoroContext::record_yield_reason(
    patronus::AsyncContext<patronus::RPC> *rpc_context,
    patronus::BaseMessage *msg)
{
    DCHECK(rpc_context->coro_state == nullptr ||
           rpc_context->coro_state == coro_state())
        << "** try to overwrite rpc_context->coro_state: "
        << rpc_context->coro_state;
    rpc_context->coro_state = coro_state();

    state_.record_yield_reason(rpc_context, msg);
}
void CoroContext::match_yield_reason(void *wr_id)
{
    state_.match_yield_reason(wr_id);
}
void CoroState::record_yield_reason(
    patronus::AsyncContext<patronus::RPC> *rpc_context,
    patronus::BaseMessage *msg)
{
    DCHECK_EQ(rpc_context->coro_id, coro_id);
    DCHECK_EQ(rpc_context->coro_state, this);
    DCHECK_EQ(wait_reason, CoroWaitReason::kRunning);
    wait_reason = CoroWaitReason::kRPC;
    auto send_type = msg->type;
    expect_resp_type = patronus::get_resp_rpc_type(send_type);
}

void CoroState::match_yield_reason(
    patronus::AsyncContext<patronus::RPC> *rpc_context,
    patronus::BaseMessage *msg)
{
    DCHECK_EQ(rpc_context->coro_id, coro_id);
    DCHECK_EQ(rpc_context->tid, tid);
    DCHECK_EQ(wait_reason, CoroWaitReason::kRPC);
    wait_reason = CoroWaitReason::kRunning;
    DCHECK_EQ(expect_resp_type, msg->type);
}

void CoroContext::wait_yield_to_master()
{
    DCHECK_NOTNULL(cb_)->wait(this);
    yield_to_master();
}

void CoroContext::yield_to_master()
{
    DLOG_IF(INFO, ::config::kMonitorCoroSwitch)
        << "[Coro] " << *this << " yielding to master";
    DCHECK(is_worker()) << *this;

    (*yield_)(*master_);

    DLOG_IF(INFO, ::config::kMonitorCoroSwitch)
        << "[Coro] " << *this << " comming back from master";
    // when come back, only two situations are allowed
    // state_wait_reason == kRunning
    // - a) no record_yield_reason() and match_yield_reason() is involved.
    // - b) record_yield_reson() and match_yield_reason() called and match
    // perfectly.
    // state_wait_reason == kIdle
    // - a) more works are given to the worker.
    CHECK(state_.wait_reason == CoroWaitReason::kRunning ||
          state_.wait_reason == CoroWaitReason::kIdle)
        << "** Unexpected state: " << state_.wait_reason << ": " << *this;
    if (state_.wait_reason == CoroWaitReason::kIdle)
    {
        state_.wait_reason = CoroWaitReason::kRunning;
    }
}
void CoroContext::yield_to_worker(coro_t wid)
{
    DLOG_IF(INFO, ::config::kMonitorCoroSwitch)
        << "[Coro] " << *this << " yielding to worker " << (int) wid;
    DCHECK(is_master()) << *this;

    (*yield_)(workers_[wid]);

    DLOG_IF(INFO, ::config::kMonitorCoroSwitch)
        << "[Coro] " << *this << " yielding back from worker " << (int) wid;
}
CoroContext::~CoroContext()
{
    if (is_worker())
    {
        if (unlikely(state_.wait_reason != CoroWaitReason::kExited))
        {
            LOG(WARNING) << "[coro] Coroutine exits in state: "
                         << state_.wait_reason;
        }
    }
}

std::optional<coro_t> CoroContext::next_hot_waiting_coro()
{
    DCHECK(is_master());
    return DCHECK_NOTNULL(cb_)->next_waiting_coro();
}

bool CoroContext::is_hot_waiting_coro(coro_t coro_id) const
{
    return DCHECK_NOTNULL(cb_)->is_wait_condition(coro_id);
}

void CoroContext::set_trace(trace_t trace)
{
    trace_ = trace;
}
trace_t CoroContext::trace() const
{
    return trace_;
}
ContTimer<::config::kEnableTrace> &CoroContext::timer()
{
    return timer_;
}
CoroState *CoroContext::coro_state()
{
    return &state_;
}
const CoroState *CoroContext::coro_state() const
{
    return &state_;
}

std::ostream &operator<<(std::ostream &os, const CoroState &s)
{
    os << "{CoroState tid: " << s.tid << ", coro_id: " << s.coro_id
       << ", wait_reason: " << s.wait_reason
       << ", expect_resp_type: " << s.expect_resp_type
       << ", expect_wr_id: " << patronus::WRID(s.expect_wr_id) << "}";
    return os;
}

void CoroControlBlock::wait(CoroContext *ctx)
{
    auto coro_id = ctx->coro_id();
    DCHECK_EQ(wait_condition_[coro_id], nullptr);
    DCHECK_LT(coro_id, define::kMaxCoroNr);
    wait_coros_.emplace_back(coro_id);
    wait_condition_[coro_id] = ctx->wait_variable();
}

std::optional<coro_t> CoroControlBlock::next_waiting_coro()
{
    for (auto it = wait_coros_.begin(); it != wait_coros_.end(); /*  */)
    {
        coro_t coro_id = (*it);
        if (DCHECK_NOTNULL(wait_condition_[coro_id])
                ->load(std::memory_order_acq_rel))
        {
            it = wait_coros_.erase(it);
            wait_condition_[coro_id] = nullptr;
            return coro_id;
        }
        else
        {
            it++;
        }
    }
    return std::nullopt;
}

bool CoroControlBlock::is_wait_condition(coro_t coro_id) const
{
    return wait_condition_[coro_id] != nullptr;
}