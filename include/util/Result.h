#pragma once
#include <exception>
#include <iostream>

#include "util/Likely.h"

namespace util
{
namespace details
{
struct result_tag_t
{
};
struct error_tag_t
{
};

}  // namespace details

class bad_result_access : public std::exception
{
};

template <typename ResultT, typename ErrorT>
class Result
{
public:
    using Self = Result<ResultT, ErrorT>;

    Result(const Result &) = default;
    Result(Result &&) = default;
    template <typename... Args>
    constexpr Result(details::result_tag_t, Args &&...args)
        : result_(std::forward<Args>(args)...), error_(), is_result_(true)
    {
    }
    template <typename... Args>
    constexpr Result(details::error_tag_t, Args &&...args)
        : result_(), error_(std::forward<Args>(args)...), is_result_(false)
    {
    }

    // for result only
    constexpr Result(const ResultT &result) : result_(result), is_result_(true)
    {
    }
    constexpr Result(ResultT &&result) noexcept(
        std::is_nothrow_move_constructible_v<ResultT>
            &&std::is_nothrow_move_constructible_v<ErrorT>)
        : result_(std::move(result)), is_result_(true)
    {
    }

    constexpr Self &operator=(const Self &) = default;
    constexpr Self &operator=(Self &&) noexcept(
        std::is_nothrow_move_assignable_v<ResultT>
            &&std::is_nothrow_move_constructible_v<ResultT>
                &&std::is_nothrow_move_assignable_v<ErrorT>
                    &&std::is_nothrow_move_constructible_v<ErrorT>) = default;
    // for result only
    constexpr Self &operator=(const ResultT &result)
    {
        result_ = result;
        is_result_ = true;
        return *this;
    }
    constexpr Self &operator=(ResultT &&result)
    {
        result_ = std::move(result);
        is_result_ = true;
        return *this;
    }

    constexpr ResultT &result() &
    {
        if (unlikely(!is_result_))
        {
            throw bad_result_access{};
        }
        return result_;
    }
    constexpr const ResultT &result() const &
    {
        if (unlikely(!is_result_))
        {
            throw bad_result_access{};
        }
        return result_;
    }
    constexpr ResultT &&result() &&
    {
        if (unlikely(!is_result_))
        {
            throw bad_result_access{};
        }
        return std::move(result_);
    }
    constexpr const ResultT &&result() const &&
    {
        if (unlikely(!is_result_))
        {
            throw bad_result_access{};
        }
        return std::move(result_);
    }

    constexpr ErrorT &error() &
    {
        if (unlikely(is_result_))
        {
            throw bad_result_access{};
        }
        return error_;
    }
    constexpr const ErrorT &error() const &
    {
        if (unlikely(is_result_))
        {
            throw bad_result_access{};
        }
        return error_;
    }
    constexpr ErrorT &&error() &&
    {
        if (unlikely(is_result_))
        {
            throw bad_result_access{};
        }
        return std::move(error_);
    }
    constexpr const ErrorT &&error() const &&
    {
        if (unlikely(is_result_))
        {
            throw bad_result_access{};
        }
        return std::move(error_);
    }

    template <typename... Args>
    constexpr ResultT result_or(Args &&...args) const &
    {
        if (is_result_)
        {
            return result_;
        }
        return ResultT(std::forward<Args>(args)...);
    }
    template <typename... Args>
    constexpr ResultT result_or(Args &&...args) &&
    {
        if (is_result_)
        {
            return std::move(result_);
        }
        return ResultT(std::forward<Args>(args)...);
    }

    constexpr explicit operator bool() const noexcept
    {
        return is_result_;
    }
    constexpr bool has_result() const noexcept
    {
        return is_result_;
    }
    constexpr const ResultT *operator->() const noexcept
    {
        return &result_;
    }
    constexpr ResultT *operator->() noexcept
    {
        return &result_;
    }
    constexpr const ResultT &operator*() const &noexcept
    {
        return result_;
    }
    constexpr ResultT &operator*() &noexcept
    {
        return result_;
    }
    constexpr const ResultT &&operator*() const &&noexcept
    {
        return std::move(result_);
    }
    constexpr ResultT &&operator*() &&noexcept
    {
        return std::move(result_);
    }
    constexpr void swap(const Self &other) noexcept(
        std::is_nothrow_move_constructible_v<ResultT>
            &&std::is_nothrow_move_constructible_v<ErrorT>
                &&std::is_nothrow_swappable_v<ResultT>
                    &&std::is_nothrow_swappable_v<ErrorT>)
    {
        using std::swap;
        swap(result_, other.result_);
        swap(error_, other.error_);
        swap(is_result_, other.is_result_);
    }

    template <typename... Args>
    constexpr Self &emplace(Args &&...args)
    {
        is_result_ = true;
        result_ = ResultT(std::forward<Args>(args)...);
        return *this;
    }

    template <typename... Args>
    constexpr Self &emplace_error(Args &&...args)
    {
        is_result_ = false;
        result_ = ErrorT(std::forward<Args>(args)...);
        return *this;
    }

    ~Result() = default;

private:
    ResultT result_;
    ErrorT error_;
    bool is_result_;
};

template <typename Tx, typename Ty>
inline std::ostream &operator<<(std::ostream &os, const Result<Tx, Ty> &res)
{
    if (res.has_result())
    {
        os << "Result(" << res.result() << ")";
    }
    else
    {
        os << "Error(" << res.error() << ")";
    }
    return os;
}

template <typename Rx_, typename Ry_, typename Ex_, typename Ey_>
constexpr bool operator==(const Result<Rx_, Ex_> &lhs,
                          const Result<Ry_, Ey_> &rhs)
{
    if (lhs.has_result())
    {
        return lhs.has_result() == rhs.has_result() &&
               lhs.result() == rhs.result();
    }
    else
    {
        return lhs.has_result() == rhs.has_result() &&
               lhs.error() == rhs.error();
    }
}

template <typename Rx_, typename Ry_, typename Ex_, typename Ey_>
constexpr bool operator!=(const Result<Rx_, Ex_> &lhs,
                          const Result<Ry_, Ey_> &rhs)
{
    return !(lhs == rhs);
}

template <typename ResultT, typename... Args>
constexpr ResultT make_result(Args &&...args)
{
    return ResultT(details::result_tag_t{}, std::forward<Args>(args)...);
}
template <typename ResultT, typename... Args>
constexpr ResultT make_error(Args &&...args)
{
    return ResultT(details::error_tag_t{}, std::forward<Args>(args)...);
}

}  // namespace util

namespace std
{
template <typename Tx, typename Ty>
struct hash<util::Result<Tx, Ty>>
{
    std::size_t operator()(const util::Result<Tx, Ty> &res) const
    {
        if (res.has_result())
        {
            return std::hash<Tx>{}(res.result());
        }
        else
        {
            // UB
            return 0;
        }
    }
};
}  // namespace std