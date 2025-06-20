#pragma once
#include <city.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <type_traits>

#include "Hexdump.hpp"

namespace util
{
template <typename T>
struct Hashable
{
    uint64_t hash() const
    {
        return CityHash64((const char *) this, sizeof(T));
    }
};

template <typename T>
struct Equable
{
};

template <typename T>
inline bool operator==(const Equable<T> &lhs, const Equable<T> &rhs)
{
    const T &lhs_ = static_cast<const T &>(lhs);
    const T &rhs_ = static_cast<const T &>(rhs);
    return std::memcmp((const char *) &lhs_, (const char *) &rhs_, sizeof(T)) ==
           0;
}
template <typename T>
inline bool operator!=(const Equable<T> &lhs, const Equable<T> &rhs)
{
    return !(lhs == rhs);
}

template <typename T>
struct Dumpable
{
    util::Hexdump dump() const
    {
        return util::Hexdump((const char *) this, sizeof(T));
    }
};

template <typename T>
struct MakeUnique
{
    using Pointer = std::unique_ptr<T>;

    template <typename... Args>
    static Pointer make_ptr(Args &&...args)
    {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }
};
template <typename T>
struct MakeShared
{
    using Pointer = std::shared_ptr<T>;

    template <typename... Args>
    static Pointer make_ptr(Args &&...args)
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
};

// NOTE: ctor of T is not disabled. No way to do that
template <typename T>
struct Singleton
{
    static T &instance()
    {
        static T ins_;
        return ins_;
    }

    Singleton(const Singleton &) = delete;
    Singleton &operator=(const Singleton &) = delete;

protected:
    Singleton() = default;
};

}  // namespace util

namespace std
{
template <typename T>
requires std::is_base_of_v<util::Hashable<T>, T>
struct hash<T>
{
    std::size_t operator()(const T &h) const
    {
        return h.hash();
    }
};

}  // namespace std