#pragma once
#ifndef UTIL_LITERNALS_H_
#define UTIL_LITERNALS_H_

#include <cinttypes>
#include <cstdint>

namespace util::literals
{
// this is useful for implicite type conversion
constexpr uint64_t operator""_B(unsigned long long i)
{
    return (uint64_t) i;
}
constexpr uint64_t operator""_K(unsigned long long i)
{
    return (uint64_t) i * 1000;
}
constexpr uint64_t operator""_Ki(unsigned long long i)
{
    return (uint64_t) i * 1024;
}
constexpr uint64_t operator""_KB(unsigned long long i)
{
    return (uint64_t) i * 1_Ki;
}
constexpr uint64_t operator""_M(unsigned long long i)
{
    return (uint64_t) i * 1000 * 1_K;
}
constexpr uint64_t operator""_Mi(unsigned long long i)
{
    return (uint64_t) i * 1024 * 1_Ki;
}
constexpr uint64_t operator""_MB(unsigned long long i)
{
    return (uint64_t) i * 1_Mi;
}
constexpr uint64_t operator""_G(unsigned long long i)
{
    return (uint64_t) i * 1000 * 1_M;
}
constexpr uint64_t operator""_Gi(unsigned long long i)
{
    return (uint64_t) i * 1024 * 1_Mi;
}
constexpr uint64_t operator""_GB(unsigned long long i)
{
    return (uint64_t) i * 1_Gi;
}
constexpr uint64_t operator""_T(unsigned long long i)
{
    return (uint64_t) i * 1000 * 1_G;
}
constexpr uint64_t operator""_Ti(unsigned long long i)
{
    return (uint64_t) i * 1024 * 1_Gi;
}
constexpr uint64_t operator""_TB(unsigned long long i)
{
    return (uint64_t) i * 1_Ti;
}
constexpr uint64_t operator""_P(unsigned long long i)
{
    return (uint64_t) i * 1000 * 1_T;
}
constexpr uint64_t operator""_Pi(unsigned long long i)
{
    return (uint64_t) i * 1024 * 1_Ti;
}
constexpr uint64_t operator""_PB(unsigned long long i)
{
    return (uint64_t) i * 1_Pi;
}
}  // namespace util::literals

#endif