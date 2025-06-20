#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>

#include "PerThread.h"
#include "glog/logging.h"
#include "util/Pre.h"
#include "util/lock/RWLock.h"

namespace util::synchronize
{
namespace details
{
template <typename T>
struct LogEntry
{
    bool is_valid;
    T inner;
    char padding[8 - sizeof(bool) - sizeof(T)];

    static_assert(sizeof(bool) + sizeof(T) <= 8);

    bool atomic_fill(const LogEntry<T> &val)
    {
        uint64_t old = *(uint64_t *) this;
        uint64_t cas_val = *(uint64_t *) &val;
        auto *me = (std::atomic<uint64_t> *) (uint64_t *) this;
        return me->compare_exchange_strong(
            old, cas_val, std::memory_order_acq_rel);
    }
} __attribute__((packed));
template <typename T>
std::ostream &operator<<(std::ostream &os, const LogEntry<T> &le)
{
    if (le.is_valid)
    {
        os << "{" << util::pre(le.inner) << "}";
    }
    else
    {
        os << "nil";
    }
    return os;
}

}  // namespace details

template <typename LE, size_t SZ>
class LogSegment
{
public:
    static_assert(sizeof(LE) <= 8);
    // type of real log entry
    using PLE = details::LogEntry<LE>;

    constexpr bool empty() const
    {
        return !log_seg_.front().is_valid;
    }
    constexpr bool full() const
    {
        return log_seg_.back().is_valid;
    }
    size_t guess_size() const
    {
        auto i_guess_size = g_size_.load(std::memory_order_relaxed);
        while (i_guess_size < SZ && valid(i_guess_size))
        {
            i_guess_size++;
        }
        return i_guess_size;
    }
    PLE *fetch_available()
    {
        auto ret = guess_size();
        if (ret < SZ)
        {
            return &log_seg_[ret];
        }
        return nullptr;
    }
    bool append(const LE &le)
    {
        bool ret = do_append(le);
        if (ret)
        {
            g_size_.fetch_add(1, std::memory_order_relaxed);
        }
        return ret;
    }
    bool do_append(const LE &le)
    {
        PLE ple;
        ple.inner = le;
        ple.is_valid = true;
        while (true)
        {
            auto *slot = fetch_available();
            // full
            if (!slot)
            {
                return false;
            }
            bool succ = slot->atomic_fill(ple);
            if (succ)
            {
                return true;
            }
            // else, try again
        }
    }
    template <typename LE_, size_t SZ_>
    friend std::ostream &operator<<(std::ostream &,
                                    const LogSegment<LE_, SZ_> &);

private:
    std::array<PLE, SZ> log_seg_{};
    // mutable Perthread<size_t> cache_size_{};
    std::atomic<size_t> g_size_{};

    bool valid(size_t idx) const
    {
        return log_seg_[idx].is_valid;
    }
};

template <typename LE, size_t SZ>
std::ostream &operator<<(std::ostream &os, const LogSegment<LE, SZ> &seg)
{
    for (size_t i = 0; i < SZ; ++i)
    {
        os << util::pre(seg.log_seg_[i]) << ", ";
    }
    return os;
}

template <typename LE, size_t SZ>
class CLog
{
public:
    using LogSegment = LogSegment<LE, SZ>;
    CLog()
    {
        segments_.emplace_back();
    }

    bool append(const LE &le)
    {
        auto &cur_segment = segments_.back();
        if (cur_segment.append(le))
        {
            return true;
        }

        // segment full, try to allocate new one
        UniqueGuard<RWLock> guard(rw_lock_);
        {
            auto &cur_segment2 = segments_.back();
            if (cur_segment2.append(le))
            {
                return true;
            }
            segments_.emplace_back();
            auto &real_cur_segment = segments_.back();
            CHECK(real_cur_segment.append(le));
            return true;
        }
    }
    template <typename LE_, size_t SZ_>
    friend std::ostream &operator<<(std::ostream &, const CLog<LE_, SZ_> &);

private:
    std::list<LogSegment> segments_;
    util::RWLock rw_lock_;
};

template <typename LE, size_t SZ>
std::ostream &operator<<(std::ostream &os, const CLog<LE, SZ> &clog)
{
    os << "{CLog ";
    size_t nr = 0;
    for (auto it = clog.segments_.rbegin(); it != clog.segments_.rend(); ++it)
    {
        os << "[";
        os << util::pre(*it);
        os << "]" << std::endl;
        if (nr++ >= 10)
        {
            break;
        }
    }
    os << "}";
    return os;
}
}  // namespace util::synchronize