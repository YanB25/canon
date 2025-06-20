#include <chrono>
#include <string>
#include <thread>

#include "DSMConfig.h"
#include "GlobalAddress.h"
#include "HugePageAlloc.h"
#include "Rdma.h"
#include "Timer.h"
#include "avis/DTxn.h"
#include "avis/avis.h"
#include "avis/config.h"
#include "avis/provider.h"
#include "avis/sizeclass.h"
#include "bench/experiment.h"
#include "bench/request.h"
#include "bench/trace_request.h"
#include "thirdparty/racehashing/hashtable.h"
#include "thirdparty/racehashing/utils.h"
#include "twitter_cache/parser.h"
#include "util/Hexdump.hpp"
#include "util/PerformanceReporter.h"
#include "util/PreUtil.h"
#include "util/ProcessMem.h"
#include "util/Rand.h"
#include "util/concept.h"
#include "util/gflags_dec.h"
#include "util/gflags_def.h"
#include "util/stacktrace.h"

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    DSMConfig config;
    auto dsm = DSM::getInstance(config);
    dsm->registerThread();

    // OnePassBucketMonitor<uint64_t> col(0, 100_M, 1);

    // bench::PercentileLoader loader("ETC.csv");
    bench::PercentileLoader loader("IBM.csv");
    // uint64_t total_size = 0;
    uint64_t nr = 100_M;
    uint64_t total_size = 0;
    uint64_t obj_size = 0;
    uint64_t BP_size = 0;
    for (size_t i = 0; i < nr; ++i)
    {
        auto n = loader.next();
        total_size += n + 8;
        BP_size += 8;
        obj_size += n;
        // // LOG(INFO) << n;
        // col.collect(n);
        // total_size += n;
    }
    LOG(INFO) << PRE(total_size, obj_size, BP_size) << ", BP consumption is "
              << util::pre_pcnt(1.0 * BP_size / obj_size)
              << ", raw: " << 1.0 * BP_size / obj_size;

    // LOG(INFO) << PRE(col);
    // size_t avg_size = total_size / nr;
    // LOG(INFO) << PRE(avg_size) << ", " << util::pre_byte(avg_size);

    for (size_t i = 0; i < 1_K; ++i)
    {
        dsm->alloc(4_KB);
    }

    // for (double p = 0; p < 1; p += 0.01)
    // {
    //     auto tail = col.percentile(p);
    //     LOG_IF(INFO, tail) << PRE(p, *tail);
    // }
    // double p = 1;
    // LOG(INFO) << PRE(p, col.max());
    // LOG(INFO) << loader.debug().lower_bound(0.995)->second;

    // bench::RequestGenerator::Config c{
    //     .put_get_del{1, 0, 0},
    //     .key_size_dist{{8, 1.0}},
    //     .value_size_dist = {},
    //     .value_size_model =
    //         std::vector<std::pair<size_t, double>>{
    //             {1_KB, 0.15},
    //             {10_KB, 0.3},
    //             {100_KB, 0.28},
    //             {1_MB, 0.12},
    //             {10_MB, 0.12},
    //             {100_MB, 0.03},
    //         },
    //     .z = 0,
    //     .key_rng = 10_M,
    // };

    // bench::RequestGenerator generator(c);
    // OnePassBucketMonitor<uint64_t> col(0, 100_M, 1);

    // for (size_t i = 0; i < 100_K; ++i)
    // {
    //     auto n = generator.next_value_size();
    //     // LOG(INFO) << n;
    //     col.collect(n);
    // }

    // LOG(INFO) << PRE(col);

    // for (double p = 0; p < 1; p += 0.01)
    // {
    //     auto tail = col.percentile(p);
    //     LOG_IF(INFO, tail) << PRE(p, *tail);
    // }
    // double p = 1;
    // LOG(INFO) << PRE(p, col.max());

    return 0;
}
