#include <city.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <thread>
#include <vector>

#include "Timer.h"
#include "sherman/Tree.h"
#include "util/Util.h"
#include "util/gflags_def.h"
#include "util/zipf.h"

using namespace sherman;
//////////////////// workload parameters /////////////////////

constexpr static bool kUseCoro = true;
const int kCoroCnt = 3;

int kReadRatio;
int kThreadCount;
int kNodeCount;
// uint64_t kKeySpace = 64_MB;
uint64_t kKeySpace = 16_MB;
double kWarmRatio = 0.8;

DEFINE_double(z, 0.99, "The skewness.");

//////////////////// workload parameters /////////////////////

std::thread th[kMaxAppThread];
uint64_t tp[kMaxAppThread][8];

// extern uint64_t latency[kMaxAppThread][LATENCY_WINDOWS];
uint64_t latency_th_all[LATENCY_WINDOWS];

Tree *tree;
DSM::pointer dsm;

inline Key to_key(uint64_t k)
{
    return (CityHash64((char *) &k, sizeof(k)) + 1) % kKeySpace;
}

class RequsetGenBench : public RequstGen
{
public:
    RequsetGenBench(int coro_id, DSM *dsm, int id)
        : coro_id(coro_id), dsm(dsm), id(id)
    {
        seed = util::rdtsc();
        mehcached_zipf_init(&state,
                            kKeySpace,
                            FLAGS_z,
                            (util::rdtsc() & (0x0000ffffffffffffull)) ^ id);
    }

    Request next() override
    {
        Request r;
        uint64_t dis = mehcached_zipf_next(&state);

        r.k = to_key(dis);
        r.v = 23;
        r.is_search = rand_r(&seed) % 100 < kReadRatio;

        tp[id][0]++;

        return r;
    }

private:
    [[maybe_unused]] int coro_id;
    [[maybe_unused]] DSM *dsm;
    int id;

    unsigned int seed;
    struct zipf_gen_state state;
};

RequstGen *coro_func(int coro_id, DSM *dsm, int id)
{
    return new RequsetGenBench(coro_id, dsm, id);
}

Timer bench_timer;
std::atomic<int64_t> warmup_cnt{0};
std::atomic_bool ready{false};
void thread_run(int id, ::bench::StopToken::pointer token)
{
    bindCore(id);

    dsm->registerThread();

    uint64_t all_thread = kThreadCount * dsm->getClusterSize();
    uint64_t my_id = kThreadCount * dsm->getMyNodeID() + id;

    LOG(INFO) << "I am " << my_id;

    if (id == 0)
    {
        bench_timer.begin();
    }

    uint64_t end_warm_key = kWarmRatio * kKeySpace;
    for (uint64_t i = 1; i < end_warm_key; ++i)
    {
        if (i % all_thread == my_id)
        {
            tree->insert(to_key(i), i * 2);
        }
    }

    warmup_cnt.fetch_add(1);

    if (id == 0)
    {
        while (warmup_cnt.load() != kThreadCount)
            ;
        LOG(INFO) << "node " << dsm->get_node_id() << " finished.";
        dsm->keeper_barrier("warm_finish", 1s);

        uint64_t ns = bench_timer.end();
        LOG(INFO) << "warmup time: " << util::pre_ns(ns);

        tree->index_cache_statistics();
        tree->clear_statistics();

        ready = true;

        warmup_cnt.store(0);
    }

    while (warmup_cnt.load() != 0)
    {
    }

    if constexpr (kUseCoro)
    {
        /// with coro
        tree->run_coroutine(
            coro_func, id, kCoroCnt, id == 0 /* is_master */, token);
    }
    else
    {
        /// without coro
        unsigned int seed = util::rdtsc();
        struct zipf_gen_state state;
        mehcached_zipf_init(&state,
                            kKeySpace,
                            FLAGS_z,
                            (util::rdtsc() & (0x0000ffffffffffffull)) ^ id);

        Timer timer;
        while (likely(!token->stop_requested()))
        {
            uint64_t dis = mehcached_zipf_next(&state);
            uint64_t key = to_key(dis);

            Value v;
            timer.begin();

            if (rand_r(&seed) % 100 < kReadRatio)
            {  // GET
                tree->search(key, v);
            }
            else
            {
                v = 12;
                tree->insert(key, v);
            }

            token->complete_task(1);

            auto us_10 = timer.end() / 100;
            if (us_10 >= LATENCY_WINDOWS)
            {
                us_10 = LATENCY_WINDOWS - 1;
            }
            latency[id][us_10]++;

            tp[id][0]++;

            // if (dsm->get_thread_id() == 1)
            // {
            //     LOG_EVERY_N(INFO, 1_M)
            //         << "[debug] coro master (T=" << dsm->get_thread_id()
            //         << "): access: " << dsm->metrics();
            // }
        }
    }
}

void parse_args(int argc, char *argv[])
{
    if (argc != 3)
    {
        LOG(INFO) << "Usage: ./benchmark kReadRatio kThreadCount";
        LOG(WARNING) << "got argc: " << argc;
        for (int i = 0; i < argc; ++i)
        {
            LOG(WARNING) << "got `" << argv[i] << "`";
        }
        exit(-1);
    }

    kNodeCount = FLAGS_machine_nr;
    kReadRatio = atoi(argv[1]);
    kThreadCount = atoi(argv[2]);

    LOG(INFO) << "node_nr: " << kNodeCount << ", read_ratio: " << kReadRatio
              << ", thread_nr: " << kThreadCount << ", zipfian: " << FLAGS_z;
}

void cal_latency()
{
    uint64_t all_lat = 0;
    for (int i = 0; i < LATENCY_WINDOWS; ++i)
    {
        latency_th_all[i] = 0;
        for (int k = 0; k < kMaxAppThread; ++k)
        {
            latency_th_all[i] += latency[k][i];
        }
        all_lat += latency_th_all[i];
    }

    uint64_t th50 = all_lat / 2;
    uint64_t th90 = all_lat * 9 / 10;
    uint64_t th95 = all_lat * 95 / 100;
    uint64_t th99 = all_lat * 99 / 100;
    uint64_t th999 = all_lat * 999 / 1000;

    uint64_t cum = 0;
    for (int i = 0; i < LATENCY_WINDOWS; ++i)
    {
        cum += latency_th_all[i];

        if (cum >= th50)
        {
            LOG(INFO) << "p50 " << i / 10.0;
            th50 = -1;
        }
        if (cum >= th90)
        {
            LOG(INFO) << "p90 " << i / 10.0;
            th90 = -1;
        }
        if (cum >= th95)
        {
            LOG(INFO) << "p95 " << i / 10.0;
            th95 = -1;
        }
        if (cum >= th99)
        {
            LOG(INFO) << "p99 " << i / 10.0;
            th99 = -1;
        }
        if (cum >= th999)
        {
            LOG(INFO) << "p999 " << i / 10.0;
            th999 = -1;
            return;
        }
    }
}

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    parse_args(argc, argv);

    DSMConfig config;
    config.machineNR = kNodeCount;
    dsm = DSM::getInstance(config);

    dsm->registerThread();
    tree = new Tree(
        dsm, dsm->alloc(4_KB), TreeConfig{}, dsm->getClusterSize() - 1);

    if (dsm->getMyNodeID() == 0)
    {
        constexpr static size_t kPreload = 1024000;
        LOG(INFO) << "Starting to load " << kPreload;
        for (uint64_t i = 1; i < kPreload; ++i)
        {
            tree->insert(to_key(i), i * 2);
        }
    }

    dsm->keeper_barrier("benchmark", 1s);

    auto core = std::make_unique<::bench::StopTokenCore>();

    for (int i = 0; i < kThreadCount; i++)
    {
        th[i] = std::thread(thread_run, i, core->get_token());
    }

    while (!ready.load())
    {
    }

    timespec s, e;
    uint64_t pre_tp = 0;

    int count = 0;

    clock_gettime(CLOCK_REALTIME, &s);
    while (true)
    {
        sleep(2);
        clock_gettime(CLOCK_REALTIME, &e);
        int microseconds = (e.tv_sec - s.tv_sec) * 1000000 +
                           (double) (e.tv_nsec - s.tv_nsec) / 1000;

        uint64_t all_tp = 0;
        for (int i = 0; i < kThreadCount; ++i)
        {
            all_tp += tp[i][0];
        }
        uint64_t cap = all_tp - pre_tp;
        pre_tp = all_tp;

        uint64_t all = 0;
        uint64_t hit = 0;

        auto [total_hit, total_miss] = tree->cache_statistics();
        all = total_hit + total_miss;
        hit = total_hit;

        clock_gettime(CLOCK_REALTIME, &s);

        if (++count % 3 == 0 && dsm->getMyNodeID() == 0)
        {
            cal_latency();
        }

        double per_node_tp = cap * 1.0 / microseconds;
        uint64_t cluster_tp = *dsm->sum((uint64_t) (per_node_tp * 1000), 100s);

        LOG(INFO) << "node: " << dsm->get_node_id()
                  << ", throughput: " << per_node_tp;

        if (dsm->getMyNodeID() == 0)
        {
            LOG(INFO) << "cluster thoughput: " << cluster_tp / 1000.0;
            LOG(INFO) << "cache hit rate: " << hit * 1.0 / all;
        }
    }

    return 0;
}