#include "bench/token.h"

#include "util/Pre.h"
namespace bench
{
TokenCore::TokenCore(int64_t task_nr) : remain_(task_nr), task_nr_(task_nr)
{
}
TokenWrap TokenCore::token_wrap(size_t batch_nr, ssize_t per_sync_at_least_nr)
{
    return TokenWrap(*this, task_nr_, batch_nr, per_sync_at_least_nr);
}

void TokenCore::reset(int64_t task_nr)
{
    remain_ = task_nr;
    task_nr_ = task_nr;
}

const std::atomic<int64_t> &TokenCore::inner() const
{
    return remain_;
}
std::atomic<int64_t> &TokenCore::inner()
{
    return remain_;
}

TokenWrap::TokenWrap(TokenCore &core,
                     int64_t task_nr,
                     size_t batch_nr,
                     ssize_t min_batch_size)
    : core_(core),
      batch_nr_(batch_nr),
      min_batch_size_(min_batch_size),
      batch_remain_(0),
      guess_remain_(task_nr)
{
    update_per_sync();
    CHECK_GT(min_batch_size, 0);
}
bool TokenWrap::sub(ssize_t nr)
{
    if (likely(batch_remain_ >= nr))
    {
        batch_remain_ -= nr;
        DCHECK_GE(batch_remain_, 0);

        if constexpr (debug())
        {
            actual_sub_nr_ += nr;
        }
        return true;
    }

    // batch_remain_ not enough, can I further allocate from core_?
    if (unlikely(core_exhausted_))
    {
        return false;
    }

    DCHECK_GT(task_per_sync_, 0);
    // attempt one allocation and retry
    guess_remain_ =
        core_.inner().fetch_sub(task_per_sync_, std::memory_order_relaxed) -
        task_per_sync_;

    if (unlikely(guess_remain_ < 0))
    {
        core_exhausted_ = true;
        return false;
    }
    batch_remain_ += task_per_sync_;

    update_per_sync();

    return sub(nr);
}
const TokenCore &TokenWrap::core() const
{
    return core_;
}

bool TokenWrap::exhausted() const
{
    return core_exhausted_;
}

TokenWrap TokenWrap::clone(size_t batch_nr, size_t min_batch_size) const
{
    return core_.token_wrap(batch_nr, min_batch_size);
}

size_t TokenWrap::debug_actual_sub_nr() const
{
    return actual_sub_nr_;
}

StopToken *StopTokenCore::get_token()
{
    std::lock_guard<std::mutex> lk(mu_);
    auto proxy = std::make_unique<StopToken>(*this);
    if (unlikely(stop_requested_))
    {
        proxy->request_stop();
    }
    proxy_.emplace_back(std::move(proxy));
    return proxy_.back().get();
}

bool StopTokenCore::stop_requested() const
{
    return stop_requested_;
}
void StopTokenCore::request_stop()
{
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto &p : proxy_)
    {
        p->request_stop();
    }
    stop_requested_ = true;
}

uint64_t StopTokenCore::nr() const
{
    uint64_t sum = 0;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto &p : proxy_)
    {
        sum += p->nr();
    }
    return sum;
}

const OnePassBucketMonitor<uint64_t> &StopTokenCore::global_latency_monitor()
{
    std::lock_guard<std::mutex> lk(mu_);
    lat_m_.reset(lat_min, lat_max, lat_rng);
    for (const auto &token : proxy_)
    {
        const auto &rhs_m = token->latency_monitor();
        if (!rhs_m.empty())
        {
            lat_m_.merge(rhs_m);
        }
    }
    return lat_m_;
}

bool StopToken::stop_requested() const
{
    return stop_requested_.load(std::memory_order_relaxed);
}

bool StopToken::complete_task(ssize_t nr)
{
    complete_nr_.fetch_add(nr, std::memory_order_relaxed);
    return !stop_requested();
}

void StopToken::request_stop()
{
    stop_requested_.store(true);
}
const StopTokenCore &StopToken::core() const
{
    return core_;
}
StopTokenCore &StopToken::core()
{
    return core_;
}

StopToken::pointer StopToken::clone() const
{
    return core_.get_token();
}
int64_t StopToken::global_nr() const
{
    return core_.nr();
}

}  // namespace bench