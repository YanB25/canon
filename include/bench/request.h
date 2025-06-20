#pragma once
#include <city.h>

#include <iostream>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

#include "DSMCache.h"
#include "thirdparty/racehashing/utils.h"
#include "util/Rand.h"
#include "util/ZipRand.h"

namespace bench
{
enum ReqType
{
    kPut,
    kGet,
    kDel,
};

inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] ::bench::ReqType e)
{
    switch (e)
    {
    case ::bench::kPut:
        os << "kPut";
        break;
    case ::bench::kGet:
        os << "kGet";
        break;
    case ::bench::kDel:
        os << "kDel";
        break;
    default:
        os << "Unknown ::bench::ReqType(" << (int) e << ")";
    }
    return os;
}

class IRequestGenerator
{
public:
    using Dist = std::pair<size_t, double>;
    struct Config
    {
        std::tuple<double, double, double> put_get_del;
        std::vector<Dist> key_size_dist;
        double z;
        uint64_t key_rng;
        std::optional<size_t> limit{std::nullopt};
    };
    IRequestGenerator(const Config &c) : c_(c)
    {
        if (c_.z != 0)
        {
            auto seed = util::get_thread_id() ^ fast_pseudo_rand_int();
            zip_g_ = std::make_shared<util::ZipfianGenerator>(
                0, c.key_rng, c.z, seed);
        }
    }
    virtual ReqType next() const
    {
        double select = fast_pseudo_rand_dbl(0, 1);
        auto put_prob = std::get<0>(c_.put_get_del);
        bool is_put = select < put_prob;
        if (is_put)
        {
            return kPut;
        }
        auto get_prob = std::get<1>(c_.put_get_del);
        bool is_get = select < (put_prob + get_prob);
        if (is_get)
        {
            return kGet;
        }
        return kDel;
    }
    virtual size_t next_key_size() const
    {
        double select = fast_pseudo_rand_dbl(0, 1);
        double acc_prob = 0;
        for (const auto &[size, prob] : c_.key_size_dist)
        {
            acc_prob += prob;
            if (select <= acc_prob)
            {
                return size;
            }
        }
        return c_.key_size_dist.back().first;
    }
    virtual uint64_t next_key()
    {
        if (c_.z == 0)
        {
            return fast_pseudo_rand_int(c_.key_rng - 1);
        }
        else
        {
            return zip_g_->Next();
        }
    }
    virtual size_t next_value_size() const = 0;
    using BufferView = patronus::hash::BufferView;
    void fill_key(char *buf, size_t key_size)
    {
        uint64_t key = next_key();
        uint64_t hash_key = CityHash64((char *) &key, sizeof(key));
        // uniform
        memset(buf, 0, key_size);
        memcpy(buf, &hash_key, sizeof(hash_key));
        // LOG(INFO) << "DEBUG " << key_size;
        // fast_pseudo_fill_buf(buf, key_size);
    }
    void fill_value(char *buf, size_t value_size)
    {
        fast_pseudo_fill_buf(buf, value_size);
    }
    virtual ~IRequestGenerator() = default;

protected:
    const Config c_;
    std::shared_ptr<util::ZipfianGenerator> zip_g_;
};

// This override the distribution of value sizes
class RequestGenerator : public IRequestGenerator
{
public:
    using Dist = std::pair<size_t, double>;
    struct Config
    {
        IRequestGenerator::Config config;
        std::optional<std::vector<Dist>> value_size_dist;
        std::optional<std::vector<Dist>> value_size_model;
        static Config make_default(double put_rate,
                                   double del_rate,
                                   size_t key_size,
                                   size_t value_size,
                                   uint64_t key_rng,
                                   std::optional<size_t> limit = std::nullopt)
        {
            return Config{
                .config{
                    .put_get_del = {put_rate,
                                    1.0 - put_rate - del_rate,
                                    del_rate},
                    .key_size_dist = {{key_size, 1.0}},
                    .z = 0,  // uniform
                    .key_rng = key_rng,
                    .limit = limit,
                },
                .value_size_dist = std::vector<Dist>{{value_size, 1.0}},
                .value_size_model = {},
            };
        }
    };
    RequestGenerator(const Config &c) : IRequestGenerator(c.config), c_(c)
    {
        bool have_dict = c_.value_size_dist.has_value();
        bool have_model = c_.value_size_model.has_value();
        CHECK(have_dict ^ have_model)
            << PRE(have_dict, have_model) << ", must provide exactly one";
    }

    size_t next_value_size() const
    {
        double select = fast_pseudo_rand_dbl(0, 1);
        double acc_prob = 0;
        if (c_.value_size_dist)
        {
            for (const auto &[size, prob] : *c_.value_size_dist)
            {
                acc_prob += prob;
                if (select <= acc_prob)
                {
                    return size;
                }
            }
            return c_.value_size_dist->back().first;
        }
        else
        {
            DCHECK(c_.value_size_model);
            size_t prev_size = 0;
            for (const auto &[size, prob] : *c_.value_size_model)
            {
                acc_prob += prob;
                if (select <= acc_prob)
                {
                    // generate range from [prev_size, size]
                    return fast_pseudo_rand_int(prev_size, size);
                }
                prev_size = size;
            }
            auto last_size = c_.value_size_model->back().first;
            return fast_pseudo_rand_int(prev_size, last_size);
        }
    }

private:
    Config c_;
};

inline std::ostream &operator<<(std::ostream &os,
                                const RequestGenerator::Config &conf)
{
    auto put = std::get<0>(conf.config.put_get_del);
    auto del = std::get<2>(conf.config.put_get_del);
    os << fmt::format("put:{}-del:{}-z:{}", put, del, conf.config.z);
    return os;
}

}  // namespace bench