#pragma once

#include <glog/logging.h>

#include "util/UP.h"

constexpr bool debug()
{
#ifdef NDEBUG
    return false;
#else
    return true;
#endif
}

template <typename T>
class Debug
{
public:
#ifdef NDEBUG
    template <typename... Args>
    Debug(Args &&...)
    {
    }
#else
    template <typename... Args>
    Debug(Args &&...args) : t_(std::forward<Args>(args)...)
    {
    }
#endif
    constexpr explicit operator bool() const noexcept
    {
        return debug();
    }
    static constexpr bool has_value() noexcept
    {
        return debug();
    }
    constexpr const T *operator->() const noexcept
    {
        return get_ptr();
    }
    constexpr T *operator->() noexcept
    {
        return get_ptr();
    }
    constexpr T &operator*() &noexcept
    {
        return *get_ptr();
    }
    constexpr const T &operator*() const &noexcept
    {
        return *get_ptr();
    }
    constexpr const T &&operator*() const &&noexcept
    {
        return std::move(*get_ptr());
    }
    constexpr T &&operator*() &&noexcept
    {
        return std::move(*get_ptr());
    }
    constexpr T &value() &
    {
        return *get_ptr();
    }
    constexpr const T &value() const &
    {
        return *get_ptr();
    }
    template <typename U>
    constexpr T value_or(U &&default_value) const &
    {
        return bool(*this) ? **this
                           : static_cast<T>(std::forward<U>(default_value));
    }
    template <typename U>
    constexpr T value_or(U &&default_value) &&
    {
        return bool(*this) ? std::move(**this)
                           : static_cast<T>(std::forward<U>(default_value));
    }

    constexpr void swap([[maybe_unused]] Debug &other) noexcept
    {
#ifndef NDEBUG
        using std::swap;
        swap(t_, other.t_);
#endif
    }
    constexpr void reset() noexcept
    {
#ifndef NDEBUG
        t_.~T();
#endif
    }

    template <class... Args>
    constexpr T &emplace([[maybe_unused]] Args &&...args)
    {
#ifndef NDEBUG
        t_ = T(std::forward<Args>(args)...);
#endif
        return *get_ptr();
    }

private:
    T *get_ptr()
    {
#ifdef NDEBUG
        LOG(FATAL) << "** Unwrap with -DNDEBUG defined.";
        return nullptr;
#else
        return &t_;
#endif
    }
    const T *get_ptr() const
    {
#ifdef NDEBUG
        LOG(FATAL) << "** Unwrap with -DNDEBUG defined.";
        return nullptr;
#else
        return &t_;
#endif
    }

#ifndef NDEBUG
    T t_;
#endif
};

#ifdef NDEBUG
static_assert(sizeof(Debug<int>) == 1);
#else
static_assert(sizeof(Debug<int>) == sizeof(int));
#endif

template <typename T>
std::ostream &operator<<(std::ostream &os, const Debug<T> &d)
{
    if constexpr (debug())
    {
        os << "Debug(" << util::pre(*d) << ")";
    }
    else
    {
        os << "Debug()";
    }
    return os;
}

class pre_file_location
{
public:
    pre_file_location(const std::string &file, uint64_t line)
        : file_(file), line_(line)
    {
    }
    std::string file() const
    {
        return file_;
    }
    uint64_t line() const
    {
        return line_;
    }

private:
    std::string file_;
    uint64_t line_;
};

inline std::ostream &operator<<(std::ostream &os, const pre_file_location &f)
{
    os << f.file() << ":" << f.line();
    return os;
}

#define DEBUG_HERE pre_file_location(__FILE__, __LINE__)