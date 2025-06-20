#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "PerThread.h"

namespace util
{
class TicketLockManager;

class TicketLock
{
public:
    void reset()
    {
        ticket_lock_ = 0;
    }

    void write_lock()
    {
        auto [ticket, current] = fetch_ticket();
        while (ticket != current)
        {
            util::asms::cpu_relax();
            current = get_current();
        }
    }
    // adapt to RW lock API
    void read_lock()
    {
        return write_lock();
    }
    void write_unlock()
    {
        release_ticket();
    }
    // adapt to RW lock API
    void read_unlock()
    {
        return write_unlock();
    }
    bool try_write_lock()
    {
        // only enter when currently unlocked
        auto [ticket, current] = load_state();
        if (likely(ticket == current))
        {
            write_lock();
            return true;
        }
        else
        {
            return false;
        }
    }
    bool try_read_lock()
    {
        return try_write_lock();
    }

    friend class TicketLockManager;

private:
    std::atomic<uint64_t> ticket_lock_{};

    // [ticket, current]
    std::pair<uint32_t, uint32_t> fetch_ticket()
    {
        uint64_t ret = ticket_lock_.fetch_add(1, std::memory_order_acquire);
        uint32_t ticket = ret << 32 >> 32;
        uint32_t current = ret >> 32;
        return {ticket, current};
    }
    std::pair<uint32_t, uint32_t> load_state()
    {
        uint64_t ret = ticket_lock_.load(std::memory_order_relaxed);
        uint32_t ticket = ret << 32 >> 32;
        uint32_t current = ret >> 32;
        return {ticket, current};
    }

    uint32_t get_current()
    {
        return ticket_lock_.load(std::memory_order_relaxed) >> 32;
    }
    uint32_t get_ticket()
    {
        return ticket_lock_.load(std::memory_order_relaxed) << 32 >> 32;
    }

    void release_ticket()
    {
        ticket_lock_.fetch_add((1ull << 32), std::memory_order_release);
    }
};

// thread-safe lock implementation.
class TicketLockManager
{
public:
    TicketLockManager(size_t lock_nr) : lock_nr_(lock_nr), locks_(lock_nr)
    {
    }
    constexpr size_t lock_nr() const
    {
        return lock_nr_;
    }
    // contract: ctx and pending_queue must be "not null" or "all null"
    // simultaneously
    void acquire(uint64_t key, CoroContext *ctx, std::queue<int> *pending_queue)
    {
        auto idx = key % lock_nr();
        auto &lk = locks_[idx];

        auto [ticket, current] = lk.fetch_ticket();
        while (ticket != current)
        {
            if (ctx)
            {
                DCHECK_NOTNULL(pending_queue)->push(ctx->coro_id());

                ctx->yield_to_master();

                if constexpr (debug())
                {
                    if (!CHECK_NOTNULL(pending_queue)->empty())
                    {
                        CHECK_NE(pending_queue->front(),
                                 CHECK_NOTNULL(ctx)->coro_id());
                    }
                }
            }
            current = lk.get_current();
        }
    }

    void release(uint64_t key, CoroContext *ctx)
    {
        auto idx = key % lock_nr();
        auto &lk = locks_[idx];

        lk.release_ticket();

        std::ignore = ctx;
    }

    void reset()
    {
        for (size_t i = 0; i < locks_.size(); ++i)
        {
            locks_[i].reset();
        }
    }

private:
    size_t lock_nr_;
    AlignedVector<TicketLock> locks_;
};

}  // namespace util