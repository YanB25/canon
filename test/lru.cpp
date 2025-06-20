#include "util/LRU.h"

#include <algorithm>
#include <thread>

#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Literals.h"
#include "util/Pre.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"

using namespace util::literals;

DEFINE_uint64(count, 1_M, "Number of tests");
DEFINE_uint64(rng, 1_M, "Range of zipfian number.");
DEFINE_uint64(cache, 100, "size of lru cache");

void simple()
{
    auto keys = {1, 2, 3, 1, 2, 1, 2, 3, 2, 4, 1, 5, 2, 1};
    util::LRUCache<uint64_t, uint64_t> cache(3);
    for (auto key : keys)
    {
        cache.put(key, key, false);
        LOG(INFO) << "put " << PRE(key) << ": " << PRE(cache.inner().first);
        cache.get_nolru(1);
        LOG(INFO) << "no lru get 1: " << PRE(cache.inner().first);
        if (cache.need_evict())
        {
            LOG(INFO) << "evict: " << PRE(cache.evict()) << ": "
                      << PRE(cache.inner().first);
        }
    }

    auto evis = {1, 6, 8};
    for (auto key : evis)
    {
        auto e = cache.invalidate(key);
        LOG(INFO) << "Manualy invalidate " << PRE(key) << ": "
                  << PRE(cache.inner().first) << "(victim: " << PRE(e) << ")";
    }

    auto gs = {2, 5, 2, 8, 5};
    for (auto key : gs)
    {
        cache.get(key);
        LOG(INFO) << "get " << PRE(key) << ": " << PRE(cache.inner().first);
    }
}

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    LOG(INFO) << "Cache size: " << FLAGS_cache << ", zipfian [0, " << FLAGS_rng
              << "]. Count: " << FLAGS_count;

    simple();

    {
        util::ZipfianGenerator g(0, 1_M);

        util::LRUCache<uint64_t, uint64_t> cache(FLAGS_cache);
        // key to count
        std::unordered_map<uint64_t, uint64_t> evict_count;
        for (size_t i = 0; i < FLAGS_count; ++i)
        {
            auto k = g.Next();
            auto v = k;
            cache.put(k, v, false);
            auto evict = cache.evict();
            if (evict.has_value())
            {
                auto key = evict.value().first;
                evict_count[key]++;
            }
        }

        LOG(INFO) << "Evit: " << util::pre(evict_count, 10);
        LOG(INFO) << util::pre(cache, false, 10);
    }
}