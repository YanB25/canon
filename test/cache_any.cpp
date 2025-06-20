#include <city.h>

#include <cstdint>

#include "GlobalAddress.h"
#include "HugePageAlloc.h"
#include "Metrics.h"
#include "Timer.h"
#include "avis/DTxn.h"
#include "avis/avis.h"
#include "bench/experiment.h"
#include "util/LRU.h"
#include "util/Rand.h"
#include "util/ZipRand.h"
#include "util/concept.h"
#include "util/gflags_def.h"
#include "util/stacktrace.h"

class SCache
{
public:
    SCache(size_t cache_size, size_t partition)
        : cache_size_(cache_size), partition_(partition)
    {
        for (size_t i = 0; i < partition_; ++i)
        {
            lrus_.emplace_back(
                util::LRUCache<uint64_t, uint64_t>(cache_size_ / partition_));
        }
        LOG(INFO) << "LRU size: " << lrus_.size();
    }

    auto put(size_t lru_id, uint64_t key, uint64_t value)
    {
        auto &lru = lrus_[lru_id];
        auto ret = lru.put(key, value, false);
        if (ret)
        {
            cm_.record_write_hit();
        }
        else
        {
            cm_.record_write_miss();
        }

        lru.evict();

        return ret;
    }
    auto get(size_t lru_id, uint64_t key)
    {
        auto ret = lrus_[lru_id].get(key);
        if (ret)
        {
            cm_.record_read_hit();
        }
        else
        {
            cm_.record_read_miss();
        }
        return ret;
    }

    void run(size_t key_rng, double z, size_t test_nr, double w_rate)
    {
        auto g = std::make_unique<util::ZipfianGenerator>(0, key_rng, z);

        for (size_t i = 0; i < test_nr; ++i)
        {
            uint64_t key = g->Next();
            uint64_t key_hash = CityHash64((char *) &key, sizeof(key));
            uint64_t value = key;
            auto lru_id = key_hash % lrus_.size();

            bool is_write = fast_pseudo_bool_with_prob(w_rate);

            if (is_write)
            {
                put(lru_id, key, value);
            }
            else
            {
                get(lru_id, key);
            }
        }
        LOG(INFO) << PRE(cm_);
    }

private:
    size_t cache_size_;
    size_t partition_;

    CacheMetric cm_;

    std::vector<util::LRUCache<uint64_t, uint64_t>> lrus_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    // for (size_t partition : {1, 10, 100})
    // {
    //     LOG(INFO) << PRE(partition);
    //     SCache c(100_M, 0.99, 1_M, partition);
    //     c.run(10_M, 0.5);
    // }

    // size_t data_size = 10_M;
    // size_t cache_size = 10_K;
    // {
    //     SCache c(data_size, 0.99, cache_size, 1);
    //     c.run(data_size, 0.5);
    // }
    // {
    //     SCache c(data_size, 0.99, cache_size, cache_size / 4);
    //     c.run(data_size, 0.5);
    // }
    // {
    //     SCache c(data_size, 0.99, cache_size, cache_size / 2);
    //     c.run(data_size, 0.5);
    // }
    // {
    //     SCache c(data_size, 0.99, cache_size, cache_size);
    //     c.run(data_size, 0.5);
    // }

    // {
    //     LOG(INFO) << "Partition: " << (100_M / 8);
    //     SCache c(100_M, 0.99, 1_M, 1_M / 8);
    //     c.run(10_M, 0.5);
    // }

    // SCache c(1000, 0.99, 10, 10);
    // c.run(100, 0.5);

    {
        SCache c(10, 1);
        for (size_t i = 0; i < 10; ++i)
        {
            c.put(0, i, i);
        }
        for (size_t i = 0; i < 10; ++i)
        {
            CHECK(c.get(0, i).has_value());
        }
        CHECK(!c.get(0, 11).has_value());
    }

    {
        SCache c(10, 2);
        for (size_t i = 0; i < 10; ++i)
        {
            c.put(0, i, i);
        }
        for (size_t i = 0; i < 10; ++i)
        {
            bool has = c.get(0, i).has_value();
            LOG(INFO) << PRE(i, has);
        }
    }

    return 0;
}
