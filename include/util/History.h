#pragma once

#include <cinttypes>
#include <deque>
#include <iostream>
#include <limits>
#include <queue>

#include "PerThread.h"
#include "range/v3/action.hpp"
#include "range/v3/algorithm.hpp"
#include "range/v3/view.hpp"
#include "range/v3/view/empty.hpp"
#include "range/v3/view/for_each.hpp"
#include "range/v3/view/transform.hpp"
#include "util/Util.h"
#include "util/thread_id.h"

namespace util
{
template <typename T>
class History
{
public:
    // NOTE: emplace front so that
    // it is more friendly to views's streaming

    template <typename... Args>
    void add(Args &&...args)
    {
        his_.emplace_front(std::forward<Args>(args)...);
    }
    size_t size() const
    {
        return his_.size();
    }
    bool empty() const
    {
        return his_.empty();
    }

    const std::deque<T> &history() const
    {
        return his_;
    }
    auto take(size_t n) const
    {
        return take(n, ranges::views::all);
    }
    template <typename Fn>
    auto take(size_t n, Fn &&fn) const
    {
        return his_ | std::forward<Fn>(fn) | ranges::views::take(n);
    }

    template <typename Fn>
    auto all(Fn &&fn) const
    {
        return take(std::numeric_limits<int>::max(), std::forward<Fn>(fn));
    }
    auto all() const
    {
        return all(ranges::views::all);
    }

private:
    std::deque<T> his_;
};

// Behave like std::optional but always with inner values.
template <typename T>
struct Timed
{
    Timed(const T &t)
        : time(util::asm_rdtsc()), tid(util::get_thread_id()), t_(t)
    {
    }
    Timed(T &&t)
        : time(util::asm_rdtsc()), tid(util::get_thread_id()), t_(std::move(t))
    {
    }
    template <typename... Args>
    Timed(std::in_place_t, Args &&...args)
        : time(util::asm_rdtsc()),
          tid(util::get_thread_id()),
          t_(std::forward<Args>(args)...)
    {
    }

    uint64_t time;
    uint64_t tid;
    T t_;

    auto operator<=>(const Timed<T> &rhs) const
    {
        return time <=> rhs.time;
    }
    bool operator==(const Timed<T> &rhs) const
    {
        return time == rhs.time;
    }
    const auto &t() const
    {
        return t_;
    }
    constexpr T &operator*() &noexcept
    {
        return t_;
    }
    constexpr const T &operator*() const &noexcept
    {
        return t_;
    }
    constexpr const T &&operator*() const &&noexcept
    {
        return std::move(t_);
    }
    constexpr const T *operator->() const noexcept
    {
        return &t_;
    }
    constexpr T *operator->() noexcept
    {
        return &t_;
    }
    constexpr operator bool() noexcept
    {
        return true;
    }
};

template <typename T>
inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::util::Timed<T> &v)
{
    os << "{";
    os << "tid: " << util::pre(v.tid);
    os << ", " << util::pre(v.t_);
    os << "}";
    return os;
}

template <typename T>
using TimedHistory = History<Timed<T>>;

template <typename T>
class TL_History
{
public:
    auto &current()
    {
        return his_.current();
    }
    // NOTE: for performance reason
    // We leave the sorting to the caller
    auto all() const
    {
        return all(ranges::views::all);
    }

    template <typename Fn>
    auto all(Fn &&fn) const
    {
        return take(std::numeric_limits<int>::max(), std::forward<Fn>(fn));
    }

    // CONTRACT
    // - The return value is a container.
    template <typename Fn>
    std::vector<Timed<T>> take(size_t n, Fn &&fn) const
    {
        using namespace ranges;
        // push down the filter
        auto c = ranges::views::for_each(his_,
                                         [&fn, n](const auto &t)
                                         {
                                             // push down the filter
                                             auto view = t.get().take(
                                                 n, std::forward<Fn>(fn));
                                             return ranges::yield_from(view);
                                         }) |
                 to<std::vector>();
        // must sort here
        // before taking further operations
        ranges::sort(c, greater{});
        while (c.size() > n)
        {
            c.pop_back();
        }
        return c;
    }
    auto take(size_t n) const
    {
        return take(n, ranges::views::all);
    }

private:
    Perthread<TimedHistory<T>> his_;
};

template <typename T>
inline std::ostream &operator<<(std::ostream &os, const History<T> &his)
{
    for (const auto &data : his.history())
    {
        os << util::pre(data) << std::endl;
    }
    return os;
}

template <typename T>
inline std::ostream &operator<<(std::ostream &os, const TimedHistory<T> &his)
{
    for (const auto &data : his.history())
    {
        os << util::pre(data) << std::endl;
    }
    return os;
}
}  // namespace util