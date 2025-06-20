#include <numa.h>

#include <bit>
#include <thread>

#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Bitset.h"
#include "util/Hexdump.hpp"
#include "util/Rand.h"
#include "util/bits.h"
#include "util/gflags_def.h"

// template <typename T>
// void test()
// {
//     {
//         auto v = util::all_ones<T>();
//         CHECK_EQ(util::count_ones(v), sizeof(T) * 8) << util::pre_bin(v);
//         CHECK_EQ(util::count_zeros(v), 0) << util::pre_bin(v);
//         v = util::unset_rng<T>(v, 2, 4);
//         CHECK_EQ(util::count_ones(v), sizeof(T) * 8 - 3);
//         v = util::unset_rng<T>(v, 5, 7);
//         CHECK_EQ(util::count_ones(v), sizeof(T) * 8 - 6) << util::pre_bin(v);
//         v = util::set_rng<T>(v, 2, 4);
//         CHECK_EQ(util::count_ones(v), sizeof(T) * 8 - 3);
//         auto p = util::retrieve_rng_org_position(v, 5, 7);
//         CHECK(util::is_all_zeros<T>(p));
//     }

//     {
//         auto v = util::all_zeros<T>();
//         CHECK_EQ(util::count_ones(v), 0);
//         CHECK_EQ(util::count_zeros(v), sizeof(T) * 8);
//     }

//     {
//         auto v = util::one_at<T>(0);
//         CHECK_EQ(util::count_ones(v), 1);
//         v = util::one_at<T>(sizeof(T) * 8 - 1);
//         CHECK_EQ(util::count_ones(v), 1);
//         v = util::ones_lower_n<T>(1);
//         CHECK_EQ(util::count_ones(v), 1);
//         v = util::ones_upper_n<T>(1);
//         CHECK_EQ(util::count_ones(v), 1);
//     }

//     {
//         auto t = util::ones_rng<T>(0, 0);
//         LOG(INFO) << util::pre_bin(t);
//         LOG(INFO) << PRE(t & 1);
//     }

//     {
//         auto lower = 3;
//         auto upper = 5;
//         auto t = util::ones_rng<T>(lower, upper);
//         LOG(INFO) << util::pre_bin(t) << std::endl
//                   << PRE(__builtin_ffs(t)) << std::endl
//                   << PRE(__builtin_clz(t)) << std::endl
//                   << PRE(__builtin_ffs(~t)) << std::endl
//                   << PRE(__builtin_clz(~t)) << std::endl
//                   << PRE(sizeof(uint32_t) * 8 - __builtin_clz(t)) <<
//                   std::endl
//                   << PRE(util::first_set_bit_lsb(t)) << std::endl
//                   << PRE(util::first_set_bit_msb(t)) << std::endl
//                   << PRE(util::first_unset_bit_lsb(t))
//                   << std::endl
//                   //   << PRE(util::first_unset_bit_msb(t)) << std::endl
//                   << PRE(util::count_ones(t)) << ", "
//                   << PRE(util::count_zeros(t));

//         CHECK_EQ(util::first_set_bit_msb(t), upper) << util::pre_bin(t);
//         CHECK_EQ(util::first_set_bit_lsb(t), lower) << util::pre_bin(t);
//         CHECK_EQ(util::first_unset_bit_lsb(t), 0) << util::pre_bin(t);
//         auto cnt = upper - lower + 1;
//         CHECK_EQ(util::count_ones(t), cnt) << util::pre_bin(t);
//         CHECK_EQ(util::count_zeros(t), sizeof(T) * 8 - cnt) <<
//         util::pre_bin(t);
//     }
// }

// template <typename T>
// void explain(T t)
// {
//     LOG(INFO) << util::pre_bin(t)
//               << std::endl
//               //   << PRE(__builtin_ffs(t)) << std::endl
//               //   << PRE(__builtin_clz(t)) << std::endl
//               //   << PRE(__builtin_ffs(~t)) << std::endl
//               //   << PRE(__builtin_clz(~t)) << std::endl
//               //   << PRE(sizeof(uint32_t) * 8 - __builtin_clz(t)) <<
//               std::endl
//               << PRE(util::first_set_bit_lsb(t)) << std::endl
//               << PRE(util::first_set_bit_msb(t)) << std::endl
//               << PRE(util::first_unset_bit_lsb(t)) << std::endl
//               << PRE(util::first_unset_bit_msb(t)) << std::endl
//               << PRE(util::count_ones(t)) << ", " <<
//               PRE(util::count_zeros(t));
// }

class Validator
{
public:
    Validator() : view_(inner_, sizeof(inner_))
    {
        memset(inner_, 0, sizeof(inner_));
    }

    void run()
    {
        validate();
        LOG(INFO) << "Validating set...";
        while (!bs_.all())
        {
            auto bit = fast_pseudo_rand_int(0, bit_nr() - 1);
            set(bit);
        }
        LOG(INFO) << "Validating unset...";
        while (!bs_.none())
        {
            auto bit = fast_pseudo_rand_int(0, bit_nr() - 1);
            reset(bit);
        }
    }

    void set(size_t pos)
    {
        bs_.set(pos);
        view_.set(pos);
        validate();
    }
    void reset(size_t pos)
    {
        bs_.reset(pos);
        view_.reset(pos);
        validate();
    }

    constexpr static size_t bit_nr()
    {
        return sizeof(inner_) * 8;
    }
    void validate() const
    {
        CHECK_EQ(bs_, view_);
        CHECK_EQ(bs_.count(), view_.count());
        CHECK_EQ(bs_.all(), view_.all());
        CHECK_EQ(bs_.any(), view_.any());
        CHECK_EQ(bs_.none(), view_.none());
        CHECK_EQ(bs_.size(), view_.size_bits());
        CHECK_EQ(view_.count_ones(), view_.count());
        CHECK_EQ(view_.count_ones() + view_.count_zeros(), bit_nr());
        CHECK_EQ(view_.popcount(), view_.count());

        for (size_t i = 0; i < bit_nr(); ++i)
        {
            CHECK_EQ(bs_.test(i), view_.test(i));
            CHECK_EQ(view_.is_set(i), view_.test(i));
            CHECK_NE(view_.is_set(i), view_.is_unset(i));
        }

        auto right_first_unset = view_.firstr_unset();
        auto right_first_set = view_.firstr_set();
        auto left_first_unset = view_.firstl_unset();
        auto left_first_set = view_.firstl_set();
        auto countr_zero = view_.countr_zero();
        auto countr_one = view_.countr_one();
        auto countl_zero = view_.countl_zero();
        auto countl_one = view_.countl_one();
        for (size_t i = 0; i < bit_nr(); ++i)
        {
            auto t = view_.test(i);
            if (i < right_first_unset)
            {
                CHECK(t);
            }
            if (i < right_first_set)
            {
                CHECK(!t);
            }
            if (i > left_first_set)
            {
                CHECK(!t);
            }
            if (i > left_first_unset)
            {
                CHECK(t);
            }
            if (i < countr_zero)
            {
                CHECK(!t);
            }
            if (i < countr_one)
            {
                CHECK(t);
            }
            auto lid = bit_nr() - i;
            if (lid < countl_zero)
            {
                CHECK(!t);
            }
            if (lid < countl_one)
            {
                CHECK(t);
            }
        }
    }

    auto to_bitset(void *buf, size_t size)
    {
        std::bitset<sizeof(uint64_t) * 4 * 8> bs;
        auto v = util::BitsView(buf, size);
        for (size_t i = 0; i < size * 8; ++i)
        {
            if (v.test(i))
            {
                bs.set(i);
            }
        }
        return bs;
    }

    void reset()
    {
        memset(inner_, 0, sizeof(inner_));
        view_.reset();
        bs_.reset();
    }

    void validate_op()
    {
        size_t op = 0;
        while (!bs_.all())
        {
            validate_or();
            op++;
            validate_flip();
            op++;
        }
        while (!bs_.none())
        {
            validate_and();
            op++;
            validate_flip();
            op++;
        }
        for (size_t i = 0; i < 100; ++i)
        {
            validate_xor();
            op++;
            validate_flip();
            op++;
        }
        LOG(INFO) << "PASS validate_op. " << PRE(op);
    }

    void validate_and()
    {
        uint64_t targets[4];
        for (size_t i = 0; i < 4; ++i)
        {
            targets[i] = fast_pseudo_rand_int();
        }
        auto target_bitset = to_bitset(targets, sizeof(targets));
        auto target_view = util::BitsView(targets, sizeof(targets));
        view_ &= target_view;
        bs_ &= target_bitset;
        validate();
        // if (unlikely(view_ != bs_))
        // {
        //     LOG(FATAL) << PRE(view_.size(), bs_.size()) << std::endl
        //                << view_.hex_dump() << std::endl
        //                << util::BitsView(bs_).hex_dump();
        // }
    }
    void validate_xor()
    {
        uint64_t targets[4];
        for (size_t i = 0; i < 4; ++i)
        {
            targets[i] = fast_pseudo_rand_int();
        }
        auto target_bitset = to_bitset(targets, sizeof(targets));
        view_ ^= target_bitset;
        bs_ ^= target_bitset;
        validate();
        // if (unlikely(view_ != bs_))
        // {
        //     LOG(FATAL) << PRE(view_.size(), bs_.size()) << std::endl
        //                << view_.hex_dump() << std::endl
        //                << util::BitsView(bs_).hex_dump();
        // }
    }
    void validate_flip()
    {
        view_.flip();
        bs_.flip();
        validate();
        view_.flip();
        bs_.flip();
        validate();
    }
    void validate_or()
    {
        uint64_t targets[4];
        for (size_t i = 0; i < 4; ++i)
        {
            targets[i] = fast_pseudo_rand_int();
        }
        auto target_bitset = to_bitset(targets, sizeof(targets));
        auto target_view = util::BitsView(targets, sizeof(targets));
        view_ |= target_view;
        bs_ |= target_bitset;
        validate();
        // CHECK_EQ(view_, bs_);
    }

private:
    util::BitsViewMut view_;
    uint64_t inner_[4];
    std::bitset<sizeof(inner_) * 8> bs_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    {
        Validator v;
        v.run();
    }
    {
        Validator v;
        v.validate_op();
    }

    // {
    //     uint64_t arr[4]{};
    //     auto bs = util::BitsView(arr, sizeof(arr));
    //     auto flip = ~bs;
    //     LOG(INFO) << std::endl << flip.hex_dump();
    // }
    {
        uint64_t arr[4]{};
        auto b1 = util::Bits(arr, sizeof(arr));
        auto b2 = util::Bits(arr, sizeof(arr));
        LOG(INFO) << std::endl << b1.hex_dump() << std::endl << b2.hex_dump();
    }

    LOG(INFO) << "PASS.";
}