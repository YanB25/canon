#pragma once
#include <byteswap.h>

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cinttypes>
#include <concepts>
#include <cstddef>
#include <type_traits>

#include "glog/logging.h"
#include "util/Hexdump.hpp"
#include "util/Likely.h"

/**
 * This module provides Bits, BitsView, and BitsViewMut class.
 *
 * @Bits
 * Bits own resources, and can be transformed to BitsView and BitsViewMut
 * by bits.view() and bits.mut_view();
 *
 * @BitsView
 * BitsView is an immutable *view* of a bit sequence.
 * It provides vairous methods to test the bits, e.g., popcount(), countr_one(),
 * has_any(), etc.
 * Since it is a view (thus lightweight), it is always passed by value (instead
 * of reference), similar to how std::string_view does.
 * Also, the mutability of the view *itself*
 * (i.e., whether it is BitsView or const BitsView) does not affect the
 * mutability of the bit sequence (always immutable).
 *
 * @BitsViewMut
 * The mutable version of BitsView. It supports modifying the bit sequence
 * in-place.
 * Note the conversion restriction:
 * OK: Bits => BitView, Bits => BitViewMut. Implicitely or Via bits.view() and
 * bits.mut_view()
 * also OK: BitViewMut => BitView. Implicitely or via mut_view.as_const()
 * NOT: BitView => BitViewMut (mutability violation)
 * NOT: const Bits => BitViewMut (mutability violation)
 */
namespace util
{

template <std::unsigned_integral T>
constexpr static int bit_nr()
{
    return sizeof(T) * 8;
}

template <typename U, typename T>
U &as(T &t) requires(sizeof(U) == sizeof(T))
{
    return (U &) t;
}
template <typename U, typename T>
const U &as(const T &t) requires(sizeof(U) == sizeof(T))
{
    return (const U &) t;
}

class Bits;
class BitsViewMut;

// BitsView is a dynamic-sized *view* of a sequence of bits
// while supporting bit-wise operations.
// CONTRACT: size must be multiple of 8
class BitsView
{
public:
    using Block = std::bitset<bit_nr<uint64_t>()>;
    static_assert(sizeof(Block) == sizeof(uint64_t),
                  "UB in this architecture.");

    BitsView(const void *buf, size_t size) : buf_(buf), size_(size)
    {
        DCHECK_EQ(size % sizeof(Block), 0);
    }
    BitsView(BitsViewMut mut);
    template <size_t Nb>
    BitsView(const std::bitset<Nb> &bs)
        : buf_((char *) &bs), size_(bs.size() / 8)
    {
    }
    BitsView(const Bits &);

    constexpr size_t size() const
    {
        return size_;
    }
    constexpr size_t size_bits() const
    {
        return size() * 8;
    }
    constexpr size_t size_block() const
    {
        return size_ / sizeof(Block);
    }
    constexpr size_t size_u64() const
    {
        return size_ / sizeof(uint64_t);
    }
    bool operator==(BitsView rhs) const
    {
        if (size_ != rhs.size_)
        {
            return false;
        }
        auto ret = memcmp(buf_, rhs.buf_, size_);
        return ret == 0;
    }

    template <std::unsigned_integral T>
    std::optional<T> to() const
    {
        if (likely(sizeof(T) >= size_))
        {
            T t{};
            memcpy(&t, buf_, std::min(sizeof(T), size()));
            return t;
        }
        return std::nullopt;
    }
    void to(void *buf2, size_t size2) const
    {
        DCHECK_GE(size2, size_) << "** conversion overflow";
        memcpy(buf2, buf_, std::min(size2, size_));
    }

    constexpr bool test(size_t bit_pos) const
    {
        auto [bl, bit] = get_pos(bit_pos);
        return bl.test(bit);
    }
    constexpr bool is_set(size_t bit_pos) const
    {
        return test(bit_pos);
    }
    constexpr bool is_unset(size_t bit_pos) const
    {
        return !is_set(bit_pos);
    }
    bool all() const noexcept
    {
        for (const auto &block : *this)
        {
            if (!block.all())
            {
                return false;
            }
        }
        return true;
    }
    bool any() const noexcept
    {
        for (const auto &block : *this)
        {
            if (block.any())
            {
                return true;
            }
        }
        return false;
    }
    bool none() const noexcept
    {
        return !any();
    }
    bool has_ones() const noexcept
    {
        return any();
    }
    bool has_zeros() const noexcept
    {
        return !all();
    }
    bool all_ones() const noexcept
    {
        return all();
    }
    bool all_zeros() const noexcept
    {
        return none();
    }
    size_t count() const noexcept
    {
        size_t cnt = 0;

        for (const auto &block : *this)
        {
            cnt += block.count();
        }
        return cnt;
    }
    size_t count_ones() const noexcept
    {
        return count();
    }
    size_t count_zeros() const noexcept
    {
        return size_bits() - count();
    }
    auto hex_dump() const
    {
        return util::Hexdump(buf_, size_);
    }
    auto bin_dump() const
    {
        return util::Bindump(buf_, size_);
    }

    size_t countr_zero() const
    {
        size_t ret = 0;
        for (size_t i = 0; i < size(); ++i)
        {
            auto z = std::countr_zero(*byte(i));
            ret += z;
            if (z != 8)
            {
                return ret;
            }
        }
        return ret;
    }
    size_t countr_one() const
    {
        size_t ret = 0;
        for (size_t i = 0; i < size(); ++i)
        {
            auto z = std::countr_one(*byte(i));
            ret += z;
            if (z != 8)
            {
                return ret;
            }
        }
        return ret;
    }
    size_t countl_zero() const
    {
        size_t ret = 0;
        for (ssize_t i = size() - 1; i >= 0; --i)
        {
            auto z = std::countl_zero(*byte(i));
            ret += z;
            if (z != 8)
            {
                return ret;
            }
        }
        return ret;
    }
    size_t countl_one() const
    {
        size_t ret = 0;
        for (ssize_t i = size() - 1; i >= 0; --i)
        {
            auto z = std::countl_one(*byte(i));
            ret += z;
            if (z != 8)
            {
                return ret;
            }
        }
        return ret;
    }
    size_t popcount() const
    {
        size_t ret = 0;
        for (const auto &block : *this)
        {
            ret += std::popcount(as<uint64_t>(block));
        }
        return ret;
    }
    size_t firstr_set() const
    {
        auto right_zeros = countr_zero();
        return right_zeros;
    }
    size_t firstr_unset() const
    {
        auto right_ones = countr_one();
        return right_ones;
    }
    size_t firstl_set() const
    {
        auto left_zeros = countl_zero();
        return size_bits() - left_zeros - 1;
    }
    size_t firstl_unset() const
    {
        auto left_ones = countl_one();
        return size_bits() - left_ones - 1;
    }
    const uint8_t *byte(size_t i = 0) const
    {
        DCHECK_LE(i, size_);
        return (uint8_t *) ((char *) buf_ + i);
    }
    const uint64_t *u64(size_t ith = 0) const
    {
        DCHECK_LE(ith * sizeof(uint64_t), size_);
        return ((uint64_t *) buf_) + ith;
    }

    const Block &block(std::size_t pos) const noexcept
    {
        DCHECK_EQ((uint64_t) buf_ % sizeof(uint64_t), 0);
        return ((Block *) buf_)[pos];
    }

    friend std::ostream &operator<<(std::ostream &os, BitsView b);

    const Block *begin() const
    {
        return &block(0);
    }
    const Block *end() const
    {
        return &block(size_block());
    }
    const char *data() const
    {
        return (const char *) buf_;
    }

protected:
    constexpr std::pair<const Block &, std::size_t> get_pos(
        std::size_t bit_pos) const noexcept
    {
        auto block_id = bit_pos / (sizeof(Block) * 8);
        auto bit_off = bit_pos % (sizeof(Block) * 8);
        return {block(block_id), bit_off};
    }
    const void *buf_;
    size_t size_;
};

class BitsViewMut : public BitsView
{
public:
    using Block = std::bitset<bit_nr<uint64_t>()>;
    static_assert(sizeof(Block) == sizeof(uint64_t),
                  "UB in this architecture.");

    BitsViewMut(void *buf, size_t size) : BitsView(buf, size)
    {
        DCHECK_EQ(size % sizeof(Block), 0);
    }
    template <size_t Nb>
    BitsViewMut(std::bitset<Nb> &bs) : BitsView((void *) &bs, bs.size() / 8)
    {
    }

    char *data() const
    {
        return (char *) buf_;
    }
    BitsView as_const() const
    {
        return BitsView(*this);
    }

    void bswap16() const
    {
        auto *u16_view = (uint16_t *) buf_;
        for (size_t i = 0; i < size_ / sizeof(uint16_t); ++i)
        {
            u16_view[i] = bswap_16(u16_view[i]);
        }
    }

    void bswap32() const
    {
        auto *u32_view = (uint32_t *) buf_;
        for (size_t i = 0; i < size_ / sizeof(uint32_t); ++i)
        {
            u32_view[i] = bswap_32(u32_view[i]);
        }
    }

    void bswap64() const
    {
        auto *u64_view = (uint64_t *) buf_;
        for (size_t i = 0; i < size_ / sizeof(uint64_t); ++i)
        {
            u64_view[i] = bswap_64(u64_view[i]);
        }
    }
    BitsViewMut operator^=(BitsView other) const noexcept
    {
        for (size_t i = 0; i < std::min(size_block(), other.size_block()); ++i)
        {
            auto &lhs = block(i);
            auto &rhs = other.block(i);
            lhs ^= rhs;
        }
        return *this;
    }

    BitsViewMut operator&=(BitsView other) const noexcept
    {
        for (size_t i = 0; i < std::min(size_block(), other.size_block()); ++i)
        {
            auto &lhs = block(i);
            auto &rhs = other.block(i);
            lhs &= rhs;
        }
        return *this;
    }

    BitsViewMut operator|=(BitsView other) const noexcept
    {
        for (size_t i = 0; i < std::min(size_block(), other.size_block()); ++i)
        {
            auto &lhs = block(i);
            auto &rhs = other.block(i);
            lhs |= rhs;
        }
        return *this;
    }
    BitsViewMut flip(std::size_t pos) const
    {
        auto [byte, bit] = get_pos(pos);
        byte.flip(bit);
        return *this;
    }
    BitsViewMut flip() const noexcept
    {
        for (auto &block : *this)
        {
            block.flip();
        }
        return *this;
    }
    BitsViewMut reset() const noexcept
    {
        for (auto &block : *this)
        {
            block.reset();
        }
        return *this;
    }
    BitsViewMut reset(size_t pos) const
    {
        auto [byte, bit] = get_pos(pos);
        byte.reset(bit);
        return *this;
    }
    BitsViewMut set() const noexcept
    {
        for (auto &block : *this)
        {
            block.set();
        }
        return *this;
    }
    BitsViewMut set(size_t pos, bool value = true) const
    {
        auto [byte, bit] = get_pos(pos);
        byte.set(bit, value);
        return *this;
    }

    Block *begin() const
    {
        return &block(0);
    }
    Block *end() const
    {
        return &block(size_block());
    }
    Block &block(std::size_t pos) const noexcept
    {
        return ((Block *) buf_)[pos];
    }

private:
    std::pair<Block &, std::size_t> get_pos(std::size_t bit_pos) const noexcept
    {
        auto block_id = bit_pos / (sizeof(Block) * 8);
        auto bit_off = bit_pos % (sizeof(Block) * 8);
        return {block(block_id), bit_off};
    }
};

static_assert(sizeof(util::BitsView) == sizeof(util::BitsViewMut));

inline BitsView::BitsView(BitsViewMut mut) : buf_(mut.data()), size_(mut.size())
{
}

inline std::ostream &operator<<(std::ostream &os, BitsView b)
{
    os << std::endl << util::Hexdump(b.data(), b.size());
    return os;
}

inline std::ostream &operator<<(std::ostream &os, BitsViewMut b)
{
    os << std::endl << util::Hexdump(b.data(), b.size());
    return os;
}

// Bits owns the bits
class Bits
{
public:
    Bits() = default;
    Bits(BitsView bits)
    {
        bits_.resize(bits.size());
        memcpy(bits_.data(), bits.data(), bits_.size());
    }
    Bits(BitsViewMut bits)
    {
        bits_.resize(bits.size());
        memcpy(bits_.data(), bits.data(), bits_.size());
    }
    Bits(void *buf, size_t size)
    {
        bits_.resize(size);
        memcpy(bits_.data(), buf, size);
    }
    char *data()
    {
        return (char *) bits_.data();
    }
    const char *data() const
    {
        return (const char *) bits_.data();
    }
    auto size() const
    {
        return bits_.size();
    }
    auto size_bits() const
    {
        return bits_.size() * 8;
    }
    BitsView view() const
    {
        return BitsView(bits_.data(), bits_.size());
    }
    BitsViewMut mut_view()
    {
        return BitsViewMut(bits_.data(), bits_.size());
    }
    auto hex_dump() const
    {
        return view().hex_dump();
    }
    auto bin_dump() const
    {
        return view().bin_dump();
    }
    Bits &operator&=(BitsView rhs)
    {
        mut_view() &= rhs;
        return *this;
    }
    Bits operator&(BitsView rhs) const
    {
        Bits result = *this;
        result &= rhs;
        return result;
    }
    Bits &operator|=(BitsView &rhs)
    {
        mut_view() |= rhs;
        return *this;
    }
    Bits operator|(BitsView &rhs) const
    {
        Bits result = *this;
        result |= rhs;
        return result;
    }
    Bits &operator^=(BitsView rhs)
    {
        mut_view() ^= rhs;
        return *this;
    }
    Bits operator^(BitsView rhs) const
    {
        Bits result = *this;
        result ^= rhs;
        return result;
    }
    Bits operator~() const
    {
        Bits result = *this;
        result.mut_view().flip();
        return result;
    }

private:
    std::vector<uint8_t> bits_;
};
inline std::ostream &operator<<(std::ostream &os, const Bits &b)
{
    os << b.view();
    return os;
}

inline BitsView::BitsView(const Bits &b) : buf_(b.data()), size_(b.size())
{
}

template <std::unsigned_integral T>
inline T all_zeros()
{
    return 0;
}
template <std::unsigned_integral T>
inline T all_ones()
{
    return ~all_zeros<T>();
}

template <std::unsigned_integral T>
inline T ones(int lower = 0, int upper = bit_nr<T>() - 1)
{
    DCHECK_LE(upper, bit_nr<T>() - 1);
    DCHECK_LE(lower, upper);

    auto one_nr = upper - lower + 1;
    T ret = (1ull << one_nr) - 1;
    return ret << lower;
}

template <std::unsigned_integral T>
inline T zeros(int lower = 0, int upper = bit_nr<T>() - 1)
{
    return ~ones<T>(lower, upper);
}

template <std::unsigned_integral T>
inline T one_at(int idx)
{
    return 1ull << idx;
}
template <std::unsigned_integral T>
inline T zero_at(int idx)
{
    return ~one_at<T>(idx);
}

template <std::unsigned_integral T>
inline T get_rng(T bits, int lower_pos, int upper_pos)
{
    auto masks = ones<T>(lower_pos, upper_pos);
    return bits & masks;
}

template <std::unsigned_integral T>
inline T get_rng_shift(T bits, int lower_pos, int upper_pos)
{
    return get_rng<T>(bits, lower_pos, upper_pos) >> lower_pos;
}

template <std::unsigned_integral T>
inline T ones_lower_n(int n)
{
    return (1ull << n) - 1;
}

template <std::unsigned_integral T>
inline T zeros_lower_n(int n)
{
    return ~ones_lower_n<T>(n);
}

template <std::unsigned_integral T>
inline T ones_upper_n(int n)
{
    auto bit_nr = sizeof(T) * 8;
    return zeros_lower_n<T>(bit_nr - n);
}
template <std::unsigned_integral T>
inline T zeros_upper_n(int n)
{
    auto bit_nr = sizeof(T) * 8;
    return ones_lower_n<T>(bit_nr - n);
}

template <std::unsigned_integral T>
inline T set(T val, int lower, int upper)
{
    T set = ones<T>(lower, upper);
    return val | set;
}
template <std::unsigned_integral T>
inline T unset(T val, int lower = 0, int upper = bit_nr<T>() - 1)
{
    T unset_msk = zeros<T>(lower, upper);
    return val & unset_msk;
}
template <std::unsigned_integral T>
inline T toggle(T val, int lower = 0, int upper = bit_nr<T> - 1)
{
    T ones = ones_rng<T>(lower, upper);
    return val ^ ones;
}
template <std::unsigned_integral T>
inline T toggle_at(T val, int pos)
{
    T one = one_at<T>(pos);
    return val ^ one;
}

template <std::unsigned_integral T, std::unsigned_integral U>
inline T set_rng_with_val(T org, int lower, int upper, U val_)
{
    // clearing [lower, upper]
    T val = (T) val_;
    org = unset<T>(org, lower, upper);
    // masking val's [lower, upper]
    auto val_mask = ones_lower_n<T>(upper - lower + 1);
    val &= val_mask;
    // set org's [lower, upper] to val
    org |= val << lower;
    return org;
}

template <std::unsigned_integral T>
inline bool is_all_zeros(T val)
{
    return val == 0;
}

template <std::unsigned_integral T>
inline bool is_all_ones(T val)
{
    return is_all_zeros<T>(~val);
}

template <std::unsigned_integral T>
inline bool has_zeros(T val)
{
    return !is_all_ones<T>(val);
}

template <std::unsigned_integral T>
inline bool has_ones(T val, int lower = 0, int upper = sizeof(T) * 8 - 1)
{
    return !is_all_zeros<T>(val, lower, upper);
}

template <std::unsigned_integral T>
inline int count_unset_bit(T val)
{
    return sizeof(T) * 8 - count_set_bit(val);
}

template <std::unsigned_integral T>
inline int firstr_unset(T val)
{
    return std::countr_one(val);
}
template <std::unsigned_integral T>
inline int firstr_set(T val)
{
    return std::countr_zero(val);
}

template <std::unsigned_integral T>
inline int firstl_unset(T val)
{
    auto left_ones = std::countl_one(val);
    return bit_nr<T>() - left_ones - 1;
}
template <std::unsigned_integral T>
inline int firstl_set(T val)
{
    auto left_zeros = std::countl_zero(val);
    return bit_nr<T>() - left_zeros - 1;
}

template <std::unsigned_integral T>
inline int count_ones(T val)
{
    return std::popcount(val);
}

inline size_t count_ones(void *buffer, size_t size)
{
    return BitsView(buffer, size).count_ones();
}

template <std::unsigned_integral T>
inline int count_zeros(T val)
{
    return sizeof(T) * 8 - count_ones(val);
}
inline size_t count_zeros(void *buffer, size_t size)
{
    return BitsView(buffer, size).count_zeros();
}

}  // namespace util