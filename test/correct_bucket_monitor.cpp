#include <algorithm>
#include <random>

#include "Common.h"
#include "gflags/gflags.h"
#include "util/PerformanceReporter.h"
#include "util/Rand.h"
#include "util/gflags_def.h"

using namespace util::literals;

constexpr static double kEpsilon = 0.0001;

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    OnePassBucketMonitor<double> m(0, 1, 0.0001);
    for (size_t i = 0; i < 10_M; ++i)
    {
        m.collect(fast_pseudo_rand_dbl(1));
    }
    LOG(INFO) << m;
    for (double i = 0; i <= 1; i += 0.1)
    {
        auto pi = *m.percentile(i);
        if (std::abs(pi - i) >= 3 * kEpsilon)
        {
            CHECK(false) << "m.percentile(" << i << ") got " << pi
                         << ", expect to be " << i << ", larger than allowed "
                         << 3 * kEpsilon;
        }
    }
    CHECK_DOUBLE_EQ(*m.percentile(0.99), 0.99);
    CHECK_DOUBLE_EQ(*m.percentile(0.999), 0.999);

    LOG(INFO) << "finished. ctrl+C to quit.";
}