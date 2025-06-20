#pragma once
#include <functional>
#include <type_traits>

template <typename T>
concept Memcpyable = std::is_trivially_copyable_v<T> && !std::is_array_v<T>;

template <typename T>
struct is_thread_safe
{
    static constexpr bool value = false;
};

template <typename T>
inline constexpr bool is_thread_safe_v = is_thread_safe<T>::value;

template <typename T>
concept ThreadSafe = is_thread_safe_v<T>;

template <typename T>
concept Hashable = requires(T a)
{
    {
        std::hash<T>{}(a)
        } -> std::convertible_to<std::size_t>;
};

template <typename T, typename U>
concept Gen = requires(T &t)
{
    {
        t()
        } -> std::convertible_to<std::optional<U>>;
};