#include <numa.h>

#include <thread>

#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/LRU.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"

constexpr static uint64_t kMaxKey = 1_K;
constexpr static size_t kCacheSize = kMaxKey / 100;
using K = uint64_t;
using V = uint64_t;

struct Spec
{
    struct TLS
    {
        std::unordered_map<K, V> map;
        util::ZipfianGenerator g{0, kMaxKey};
    };
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    using BLS = typename Base::BLS;

    Experiment() : lru(kCacheSize)
    {
    }
    __attribute__((always_inline)) int choose_action(size_t read_rate,
                                                     size_t evict_rate)
    {
        if (fast_pseudo_bool_with_prob(read_rate / 100.0))
        {
            return 0;  // read
        }
        if (fast_pseudo_bool_with_prob(evict_rate / 100.0))
        {
            return 2;  // evict
        }
        return 1;  // insert
    }
    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        auto &tlc = bls.thread();

        auto read_rate = conf.get<size_t>("read_rate");
        auto evict_rate = conf.get<size_t>("evict_rate");
        CHECK_LE(read_rate, 100);
        CHECK_LE(evict_rate, 100);
        while (likely(!token->stop_requested()))
        {
            int action = choose_action(read_rate, evict_rate);

            auto key = tlc.g.Next();
            if (action == 0)
            {
                // read
                auto res = lru.get(key);

                if (tlc.map.count(key))
                {
                    // key exists
                    auto expect_value = tlc.map[key];
                    CHECK(res.has_value());
                    CHECK_EQ(*res, expect_value);
                }
                else
                {
                    CHECK(!res.has_value());
                }
            }
            else if (action == 1)
            {
                // put
                auto value = fast_pseudo_rand_int();
                auto res = lru.put(key, value, false);
                if (tlc.map.count(key))
                {
                    // key exists
                    CHECK(res.has_value());
                    CHECK_EQ(*res, tlc.map[key]);
                }
                else
                {
                    // key not exists
                    CHECK(!res.has_value());
                }
                tlc.map[key] = value;
            }
            else
            {
                // invalidate
                auto res = lru.invalidate(key);
                if (tlc.map.count(key))
                {
                    // key exists
                    CHECK(res.has_value());
                    CHECK_EQ(*res, tlc.map[key]);
                }
                else
                {
                    // key not exists
                    CHECK(!res.has_value());
                }
                tlc.map.erase(key);
            }

            auto evict = lru.evict();
            if (evict)
            {
                auto key = evict->first;
                auto value = evict->second;
                CHECK(tlc.map.count(key));
                CHECK_EQ(tlc.map[key], value);
                tlc.map.erase(key);
            }

            token->complete_task(1);
        }
    }
    void on_thread_start_bench(BLS &bls, const Config &conf) override
    {
        auto &tlc = bls.thread();
        tlc.map.clear();

        Base::on_thread_start_bench(bls, conf);
    }

private:
    util::LRUCache<K, V> lru;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    {
        Experiment exp;
        exp.configure_monitor(1s, 10);
        ::bench::ConfigFactory f;

        f.configure_thread_nr({1});
        f.add_option<size_t>("read_rate", {50});
        f.add_option<size_t>("evict_rate", {50});

        exp.launch(f.generate_configs());
    }

    LOG(INFO) << "PASS.";
}