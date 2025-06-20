#pragma once
#include "Tree.h"
#include "util/zipf.h"

namespace sherman
{
class RequestGenBench : public RequstGen
{
public:
    RequestGenBench(int coro_id,
                    DSM *dsm,
                    int id,
                    uint64_t max_key,
                    std::vector<std::pair<size_t, double>> value_size_dist,
                    double z,
                    double read_ratio)
        : coro_id(coro_id),
          dsm(dsm),
          id(id),
          max_key_(max_key),
          value_size_dist_(value_size_dist),
          read_ratio_(read_ratio)
    {
        if (z == 0)
        {
            is_zip = false;
        }
        else
        {
            is_zip = true;
            seed = util::rdtsc();
            mehcached_zipf_init(&state,
                                max_key,
                                z,
                                (util::rdtsc() & (0x0000ffffffffffffull)) ^ id);
        }
    }

    Request next() override
    {
        if (is_zip)
        {
            return next_zip();
        }
        else
        {
            return next_uni();
        }
    }
    Request next_zip()
    {
        Request r;
        uint64_t dis = mehcached_zipf_next(&state);

        r.k = to_key(dis, max_key_);
        r.v = 23;
        r.is_search = rand_r(&seed) % 100 < read_ratio_;
        r.value_size = next_value_size();

        return r;
    }
    size_t next_value_size() const
    {
        double select = fast_pseudo_rand_dbl(0, 1);
        double acc_prob = 0;
        for (const auto &[size, prob] : value_size_dist_)
        {
            acc_prob += prob;
            if (select <= acc_prob)
            {
                return size;
            }
        }
        return value_size_dist_.back().first;
    }
    Request next_uni()
    {
        Request r;
        uint64_t dis = fast_pseudo_rand_int(max_key_);
        r.k = to_key(dis, max_key_);
        r.v = 23;
        r.is_search = (fast_pseudo_rand_int() % 100) < read_ratio_;
        r.value_size = next_value_size();
        return r;
    }
    __attribute__((always_inline)) static Key to_key(uint64_t k,
                                                     uint64_t max_key)
    {
        return (CityHash64((char *) &k, sizeof(k)) + 1) % max_key;
    }

private:
    [[maybe_unused]] int coro_id;
    [[maybe_unused]] DSM *dsm;
    [[maybe_unused]] int id;

    uint64_t max_key_;
    std::vector<std::pair<size_t, double>> value_size_dist_;
    uint64_t read_ratio_;

    unsigned int seed;
    struct zipf_gen_state state;

    bool is_zip{true};
};

}  // namespace sherman