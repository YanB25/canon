// https://stackoverflow.com/questions/16198700/using-the-extra-16-bits-in-64-bit-pointers
// a little bit modify

#pragma once
#include <limits>

#include "avis/sizeclass.h"
#include "util/Hexdump.hpp"
#ifndef PATRONUS_RACEHASHING_UTILS_H_
#define PATRONUS_RACEHASHING_UTILS_H_

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "Common.h"
#include "DSMCache.h"
#include "city.h"
#include "util/Debug.h"
#include "util/PerformanceReporter.h"
#include "util/RetCode.h"
// #include "util/TagPtr.h"

namespace patronus::hash
{
struct BufferView
{
    BufferView(void *buffer, size_t size)
        : buffer_((char *) buffer), size_(size)
    {
    }
    char *data()
    {
        return buffer_;
    }
    const char *data() const
    {
        return buffer_;
    }
    size_t size() const
    {
        return size_;
    }
    size_t length() const
    {
        return size_;
    }
    BufferView() = default;
    char *buffer_{};
    size_t size_{};

    std::string_view to_sv() const
    {
        return std::string_view(buffer_, size_);
    }
};

inline std::ostream &operator<<(std::ostream &os, const BufferView &b)
{
    size_t expect_size = std::min(b.size(), (size_t) 64);
    bool chuncated = expect_size != b.size();
    os << "{BufferView " << util::InlinedHexdump(b.data(), expect_size);
    if (chuncated)
    {
        os << "...";
    }
    os << ", size: " << b.size() << "}";
    return os;
}

using Key = BufferView;
using Value = BufferView;

inline uint64_t round_up_to_next_power_of_2(uint64_t x)
{
    return pow(2, ceil(log(x) / log(2)));
}

// the number of bits for hash value to locate directory
constexpr static size_t M = 16;
// the number of bits for hash value of fingerprint
constexpr static size_t FP = 8;
// the number of bits for hash value to calculate h1(*) and h2(*)
constexpr static size_t H = (64 - M - FP) / 2;
static_assert((64 - M - FP) % 2 == 0);

constexpr static uint64_t kRaceHashingHashSeed = 5381;

inline uint64_t hash_impl(const void *buf, size_t size)
{
    // uint64_t hash = kRaceHashingHashSeed;
    // for (size_t i = 0; i < size; ++i)
    // {
    //     hash = ((hash << 5) + hash) + buf[i]; /* hash * 33 + c */
    // }
    // return hash;
    return CityHash64((const char *) buf, size);
}

// NOTE: make FP next to M
// (higher) [ H2 | H1 | FP | M ]  (lower)

// get the bit range (upper, lower]
inline uint64_t hash_from(uint64_t h, size_t lower, size_t upper)
{
    // get the lower @upper bits
    DCHECK_LE(upper, 64);
    DCHECK_LE(lower, upper);

    if (upper != 64)
    {
        h = h & ((1ull << upper) - 1);
    }
    // get rid of the upper bits
    return h >> lower;
}
inline uint64_t hash_m(uint64_t h)
{  // the lowest M bits
    return hash_from(h, 0, M);
}
inline uint64_t hash_fp(uint64_t h)
{
    return hash_from(h, M, FP + M);
}
inline uint64_t __hash_1(uint64_t h)
{
    return hash_from(h, FP + M, FP + M + H);
}
inline uint64_t __hash_2(uint64_t h)
{
    return hash_from(h, FP + M + H, FP + M + H + H);
}
inline std::pair<uint64_t, uint64_t> hash_h1_h2(uint64_t hash)
{
    auto h1 = __hash_1(hash);
    auto h2 = __hash_2(hash);
    return {h1, h2};
}

constexpr bool is_power_of_two(uint64_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

class pre_fp
{
public:
    pre_fp(uint8_t fp) : fp_(fp)
    {
    }
    friend std::ostream &operator<<(std::ostream &os, const pre_fp &);

private:
    uint8_t fp_{0};
};
inline std::ostream &operator<<(std::ostream &os, const pre_fp &fp)
{
    auto flags = os.flags();
    os << std::hex << (int) fp.fp_;
    os.flags(flags);
    return os;
}

class pre_addr
{
public:
    pre_addr(uint64_t addr) : addr_(addr)
    {
    }
    friend std::ostream &operator<<(std::ostream &os, const pre_addr &);

private:
    uint64_t addr_{0};
};
inline std::ostream &operator<<(std::ostream &os, const pre_addr &addr)
{
    auto flags = os.flags();
    os << std::hex << (void *) addr.addr_;
    os.flags(flags);
    return os;
}

class pre_len
{
public:
    pre_len(uint8_t len) : len_(len)
    {
    }
    friend std::ostream &operator<<(std::ostream &os, const pre_len &);

private:
    uint8_t len_{0};
};
inline std::ostream &operator<<(std::ostream &os, const pre_len &len)
{
    os << (int) len.len_;
    return os;
}
class pre_hash
{
public:
    pre_hash(uint64_t hash) : hash_(hash)
    {
    }
    friend std::ostream &operator<<(std::ostream &os, const pre_hash &);

private:
    uint64_t hash_;
};
inline std::ostream &operator<<(std::ostream &os, const pre_hash &ph)
{
    auto flags = os.flags();
    os << std::hex << ph.hash_;
    os.flags(flags);
    return os;
}

class pre_suffix
{
public:
    pre_suffix(uint32_t suffix, size_t len) : suffix_(suffix), len_(len)
    {
    }
    friend std::ostream &operator<<(std::ostream &os, const pre_suffix &);

private:
    uint32_t suffix_;
    size_t len_;
};
inline std::ostream &operator<<(std::ostream &os, const pre_suffix &sfx)
{
    if (sfx.len_ <= 2)
    {
        os << sfx.suffix_ << "(" << std::bitset<2>(sfx.suffix_) << ")";
    }
    else if (sfx.len_ <= 4)
    {
        os << sfx.suffix_ << "(" << std::bitset<4>(sfx.suffix_) << ")";
    }
    else if (sfx.len_ <= 8)
    {
        os << sfx.suffix_ << "(" << std::bitset<8>(sfx.suffix_) << ")";
    }
    else if (sfx.len_ <= 12)
    {
        os << sfx.suffix_ << "(" << std::bitset<12>(sfx.suffix_) << ")";
    }
    else if (sfx.len_ <= 16)
    {
        os << sfx.suffix_ << "(" << std::bitset<16>(sfx.suffix_) << ")";
    }
    else
    {
        os << sfx.suffix_ << "(" << std::bitset<64>(sfx.suffix_) << ")";
    }
    return os;
}

inline void hash_table_free([[maybe_unused]] void *addr)
{
    LOG_FIRST_N(WARNING, 1) << "Not actually freeing. Implement rcu here.";
}

enum class RetryReason
{
    kUpdateSlot,
    kInsertSlot,
    kCacheStale,
};
inline std::ostream &operator<<(std::ostream &os, RetryReason r)
{
    switch (r)
    {
    case RetryReason::kUpdateSlot:
        os << "kUpdateSlot";
        break;
    case RetryReason::kInsertSlot:
        os << "kInsertSlot";
        break;
    case RetryReason::kCacheStale:
        os << "kCacheStale";
        break;
    }
    return os;
}

inline uint64_t round_hash_to_bit(uint32_t h, size_t bit)
{
    if (bit == 64)
    {
        return h;
    }
    return h & ((1ull << bit) - 1);
}

inline uint64_t round_to_bits(uint64_t hash, size_t bits)
{
    if (bits == 64)
    {
        return hash;
    }
    return hash & ((1ull << bits) - 1);
}

inline constexpr size_t len_to_ptr_len(size_t len)
{
    return avis::SizeClass{}.to_class_size_index(len);
}
inline constexpr size_t ptr_len_to_len(size_t ptr_len)
{
    return avis::SizeClass{}.index_to_class_size(ptr_len);
}

inline constexpr std::pair<size_t, size_t> get_actual_kvblock_tagged_size(
    size_t size)
{
    size_t tag_ptr_len = len_to_ptr_len(size);
    size_t kvblock_size = ptr_len_to_len(tag_ptr_len);
    return {kvblock_size, tag_ptr_len};
}

};  // namespace patronus::hash

#endif