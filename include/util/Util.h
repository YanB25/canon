#pragma once
#ifndef UTIL_UTIL_H_
#define UTIL_UTIL_H_
#include <boost/algorithm/string.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "Common.h"
#include "IOVerbose.h"
#include "util/Debug.h"
#include "util/Likely.h"
#include "util/gflags_dec.h"
#include "util/thread_id.h"

DECLARE_string(node_id);

#define ROUND_UP(num, multiple) ceil(((double) (num)) / (multiple)) * (multiple)

struct Data
{
    union
    {
        struct
        {
            uint32_t lower;
            uint32_t upper;
        };
        uint64_t val;
    };
} __attribute__((packed));

void bindCore(uint16_t core);
char *getIP();
char *getMac();

namespace util
{
static inline unsigned long long asm_rdtsc(void)
{
    unsigned hi, lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long) lo) | (((unsigned long long) hi) << 32);
}

__inline__ unsigned long long rdtsc(void)
{
    unsigned hi, lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long) lo) | (((unsigned long long) hi) << 32);
}

inline void mfence()
{
    asm volatile("mfence" ::: "memory");
}

inline void compiler_barrier()
{
    asm volatile("" ::: "memory");
}

static inline uint64_t djb2_digest(const void *void_str, size_t size)
{
    const char *str = (const char *) void_str;
    unsigned long hash = 5381;
    int c;

    for (size_t i = 0; i < size; ++i)
    {
        c = str[i];
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

}  // namespace util

inline std::string binary_to_csv_filename(
    const std::string &bench_path,
    const std::string &exec_meta,
    const std::map<std::string, std::string> &extra = {})
{
    std::string root = "../result/";
    auto filename = std::filesystem::path(bench_path).filename().string();

    std::string ret = root + filename + "." + exec_meta + ".";
    for (const auto &[k, v] : extra)
    {
        ret += k + ":" + v + ".";
    }
    if constexpr (debug())
    {
        ret += "DEBUG.";
    }
    ret += "csv";
    return ret;
}
inline std::filesystem::path artifacts_directory()
{
    return "../artifacts/";
}

inline void ro_prefetch([[maybe_unused]] const void *addr)
{
    // __builtin_prefetch(addr, 0 /* for read */, 1);
}

inline uint64_t round_up_div(uint64_t a, uint64_t b)
{
    return (a + b - 1) / b;
}

inline int count_one(int x)
{
    x = (x & (0x55555555)) + ((x >> 1) & (0x55555555));
    x = (x & (0x33333333)) + ((x >> 2) & (0x33333333));
    x = (x & (0x0f0f0f0f)) + ((x >> 4) & (0x0f0f0f0f));
    x = (x & (0x00ff00ff)) + ((x >> 8) & (0x00ff00ff));
    x = (x & (0x0000ffff)) + ((x >> 16) & (0x0000ffff));
    return x;
}

inline bool is_mw_magic_err(uint64_t id)
{
    constexpr static uint32_t magic = 0b1010101010;
    constexpr static uint16_t mask = 0b1111111111;
    if (unlikely((id & mask) == magic))
    {
        return true;
    }
    return false;
}

namespace util
{
inline std::optional<int> node_id_in_rank_file()
{
    if (unlikely(FLAGS_node_id.empty()))
    {
        return std::nullopt;
    }
    return std::stoi(FLAGS_node_id);
}

template <typename T>
std::vector<T> filter(const std::vector<T> &vec,
                      const std::function<bool(const T &)> &p)
{
    std::vector<T> ret;
    std::copy_if(vec.begin(), vec.end(), std::back_inserter(ret), p);
    return ret;
}
__attribute__((always_inline)) inline bool is_power_of_two(uint64_t x)
{
    // https://stackoverflow.com/questions/108318/how-can-i-test-whether-a-number-is-a-power-of-2
    bool powerOfTwo = !(x == 0) && !(x & (x - 1));
    return powerOfTwo;
}

__attribute__((always_inline)) inline bool is_aligned(uint64_t val,
                                                      size_t align)
{
    DCHECK(is_power_of_two(align))
        << "why checking an alignment not power of 2?";
    return val % align == 0;
}
template <typename T>
requires std::is_scalar_v<T>
__attribute__((always_inline)) inline T to_aligned(T addr, size_t align)
{
    DCHECK(is_power_of_two(align))
        << "why going to an alignment not power of 2?";
    uint64_t uaddr = (uint64_t) addr;
    uint64_t ret = uaddr - (uaddr % align);
    return (T) ret;
}

__attribute__((always_inline)) inline uint64_t round_up_aligned(uint64_t addr,
                                                                size_t align)
{
    DCHECK(is_power_of_two(align))
        << "why going to an alignment not power of 2?";
    if (align == 0)
    {
        return addr;
    }
    if (addr % align == 0)
    {
        return addr;
    }
    auto more = addr % align;
    auto counter_more = align - more;
    return addr + counter_more;
}

__attribute__((always_inline)) inline uint64_t round_up_next_power_of_two(
    uint64_t x)
{
    // https://stackoverflow.com/questions/466204/rounding-up-to-next-power-of-2
    return x == 1 ? 1 : 1ull << (64 - __builtin_clzll(x - 1));
}

inline void *memcpy_atomic_64b(void *dest, const void *src, size_t count)
{
    char *d = (char *) dest;
    const char *s = (const char *) src;

    // Copy 64-byte chunks
    size_t chunks = count / 64;
    for (size_t i = 0; i < chunks; ++i, s += 64, d += 64)
    {
        __asm__ __volatile__(
            "vmovdqa64 (%1), %%zmm0\n\t"
            "vmovdqa64 %%zmm0, (%0)\n\t"
            : /* no output registers */
            : "r"(d), "r"(s)
            : "memory", "zmm0");
    }

    // Copy any remaining bytes
    size_t remaining = count % 64;
    for (size_t i = 0; i < remaining; ++i)
    {
        d[i] = s[i];
    }

    return dest;
}
inline void *memcpy_atomic_64b_order(void *dest, const void *src, size_t count)
{
    char *d = (char *) dest;
    const char *s = (const char *) src;

    // Copy 64-byte chunks
    size_t chunks = count / 64;
    for (size_t i = 0; i < chunks; ++i, s += 64, d += 64)
    {
        __asm__ __volatile__(
            "vmovdqa64 (%1), %%zmm0\n\t"
            "vmovdqa64 %%zmm0, (%0)\n\t"
            "mfence\n\t"
            : /* no output registers */
            : "r"(d), "r"(s)
            : "memory", "zmm0");
    }

    // Copy any remaining bytes
    size_t remaining = count % 64;
    for (size_t i = 0; i < remaining; ++i)
    {
        d[i] = s[i];
    }

    return dest;
}

}  // namespace util

#endif