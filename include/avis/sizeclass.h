#pragma once
#include <algorithm>
#include <array>
#include <cstddef>

#include "util/Util.h"

namespace avis
{
class SizeClass
{
public:
    constexpr SizeClass() = default;

    size_t to_class_size(size_t size) const
    {
        if (unlikely(size == 0))
        {
            return size_class_.front();
        }
        // NOTE: the `size - 1`
        auto it =
            std::upper_bound(size_class_.begin(), size_class_.end(), size - 1);
        if (unlikely(it == size_class_.end()))
        {
            // no size class for this. fall back to
            return util::round_up_next_power_of_two(size);
        }
        return *it;
    }
    constexpr uint8_t to_class_size_index(size_t size) const
    {
        if (unlikely(size == 0))
        {
            return 0;
        }
        // NOTE: the `size - 1`
        auto it =
            std::upper_bound(size_class_.begin(), size_class_.end(), size - 1);
        if (unlikely(it == size_class_.end()))
        {
            // no size class for this.
            LOG(FATAL) << "No size class for " << PRE(size);
            return 0;
        }
        return it - size_class_.begin();
    }
    constexpr size_t index_to_class_size(uint8_t idx) const
    {
        return size_class_[idx];
    }
    constexpr const auto &size_class() const
    {
        return size_class_;
    }

private:
    // This array is generated from script/gen_slab.py
    static inline constexpr const std::array<size_t, 255> size_class_{
        16,       32,       40,       48,       56,       64,       72,
        80,       88,       96,       104,      112,      120,      128,
        135,      142,      150,      158,      166,      175,      184,
        194,      204,      215,      226,      238,      250,      263,
        277,      291,      306,      322,      339,      356,      374,
        393,      413,      434,      456,      479,      503,      529,
        556,      584,      614,      645,      678,      712,      748,
        786,      826,      868,      912,      958,      1006,     1057,
        1110,     1166,     1225,     1287,     1352,     1420,     1491,
        1566,     1645,     1728,     1815,     1906,     2002,     2103,
        2209,     2320,     2436,     2558,     2686,     2821,     2963,
        3112,     3268,     3432,     3604,     3785,     3975,     4174,
        4383,     4603,     4834,     5076,     5330,     5597,     5877,
        6171,     6480,     6804,     7145,     7503,     7879,     8273,
        8687,     9122,     9579,     10058,    10561,    11090,    11645,
        12228,    12840,    13482,    14157,    14865,    15609,    16390,
        17210,    18071,    18975,    19924,    20921,    21968,    23067,
        24221,    25433,    26705,    28041,    29444,    30917,    32463,
        34087,    35792,    37582,    39462,    41436,    43508,    45684,
        47969,    50368,    52887,    55532,    58309,    61225,    64287,
        67502,    70878,    74422,    78144,    82052,    86155,    90463,
        94987,    99737,    104724,   109961,   115460,   121233,   127295,
        133660,   140343,   147361,   154730,   162467,   170591,   179121,
        188078,   197482,   207357,   217725,   228612,   240043,   252046,
        264649,   277882,   291777,   306366,   321685,   337770,   354659,
        372392,   391012,   410563,   431092,   452647,   475280,   499044,
        523997,   550197,   577707,   606593,   636923,   668770,   702209,
        737320,   774186,   812896,   853541,   896219,   941030,   988082,
        1037487,  1089362,  1143831,  1201023,  1261075,  1324129,  1390336,
        1459853,  1532846,  1609489,  1689964,  1774463,  1863187,  1956347,
        2054165,  2156874,  2264718,  2377954,  2496852,  2621695,  2752780,
        2890419,  3034940,  3186687,  3346022,  3513324,  3688991,  3873441,
        4067114,  4270470,  4483994,  4708194,  4943604,  5190785,  5450325,
        5722842,  6008985,  6309435,  6624907,  6956153,  7303961,  7669160,
        8052618,  8455249,  8878012,  9321913,  9788009,  10277410, 10791281,
        11330846, 11897389, 12492259, 13116872, 13772716, 14461352, 15184420,
        15943641, 16740824, 17577866};
};
}  // namespace avis