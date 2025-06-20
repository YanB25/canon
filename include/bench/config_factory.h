// clang-format off
/**
 * @file config_factory.h
 * config_factory.h does the magic that generates a vector of configs to launch
 * tests.
 *
 * typical usage:
 * 
 *    ConfigFactory f; 
 *    f.configure_thread_nr({1, 2, 4, 8, 12});
 *    f.configure_coro_nr({1});
 *    f.add_option("read_ratio", {0, 50, 100});
 *    f.add_option("lock", {"rw", "mcas", "spin"});
 *    // this automatically do the Cartesian product to generate a vector of configs
 *    f.generate_configs():
 * 
 * Behind the screen is the magic of using std::any to store any type. 
 * Then use a map to store all the options flags.
 * 
 * What really important is that
 * 1) We use std::any to store any type. 
 * Use AnyWrapper to store <std::any, str_pre> so that we can cout annoymous items.
 * 2) We run *Cartesian product* on the multiple std::vector<AnyWrapper>
 * without knowing what actually the type is.
 * 3) We let the user to cast back std::any correctly.
 * 
 */
// clang-format on
#pragma once

#include <any>
#include <iostream>
#include <typeindex>
#include <typeinfo>
#include <utility>

#include "base_config.h"
#include "glog/logging.h"
#include "util/UP.h"

namespace bench
{
struct AnyWrapper
{
    std::any value;
    std::string pre_str;
};

enum class ToDataFrame
{
    kNo,
    kAsString,
    kAsUint64,
};
inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] ::bench::ToDataFrame e)
{
    switch (e)
    {
    case ::bench::ToDataFrame::kNo:
        os << "kNo";
        break;
    case ::bench::ToDataFrame::kAsString:
        os << "kAsString";
        break;
    case ::bench::ToDataFrame::kAsUint64:
        os << "kAsUint64";
        break;
    default:
        os << "Unknown ::bench::ToDataFrame(" << (int) e << ")";
    }
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const AnyWrapper &w)
{
    os << w.pre_str;
    return os;
}

struct QuickBenchConfig : public ::bench::IBenchConfig
{
    size_t thread_nr() const override
    {
        return get<size_t>("thread_nr");
    }
    size_t coro_nr() const override
    {
        auto ret = try_get<size_t>("coro_nr");
        if (ret)
        {
            return *ret;
        }
        return 1;
    }
    std::string name() const override
    {
        return "quick-bench";
    }

    void put(const std::string &name, const AnyWrapper &val, bool to_df)
    {
        options_.emplace(name, val);
        if (to_df)
        {
            to_df_.emplace_back(name);
        }
    }
    template <typename T>
    void do_put(const std::string &name, const T &val, bool to_df)
    {
        AnyWrapper w{.value = val, .pre_str = util::pre_str(val)};
        options_.emplace(name, w);
        if (to_df)
        {
            to_df_.emplace_back(name, to_df);
        }
    }

    template <typename T>
    T get(const std::string &name) const
    {
        auto ret = try_get<T>(name);
        CHECK(ret.has_value()) << "** " << PRE(name) << " not found in config";
        return ret.value();
    }

    template <typename T>
    std::optional<T> try_get(const std::string &name) const
    {
        auto it = options_.find(name);
        if (it == options_.end())
        {
            return std::nullopt;
        }
        try
        {
            return std::any_cast<T>(it->second.value);
        }
        catch (std::bad_any_cast)
        {
            LOG(FATAL) << "Failed to get `" << name << "` with type "
                       << typeid(T).name() << ": std::bad_any_cast. ";
            return std::nullopt;
        }
    }

    std::shared_ptr<QuickBenchConfig> clone()
    {
        auto ret = std::make_shared<QuickBenchConfig>();
        ret->options_ = options_;
        ret->to_df_ = to_df_;
        return ret;
    }
    std::list<std::pair<std::string, std::string>> df_options() const override
    {
        std::list<std::pair<std::string, std::string>> ret;
        for (const auto &name : to_df_)
        {
            auto it = options_.find(name);
            if (it == options_.end())
            {
                LOG(FATAL) << "Internal error: not found `" << name << "`";
            }
            else
            {
                ret.emplace_back(std::make_pair(name, it->second.pre_str));
            }
        }
        return ret;
    }
    const auto &to_df() const
    {
        return to_df_;
    }

    // name => value
    std::map<std::string, AnyWrapper> options_;
    std::list<std::string> to_df_;
};
inline std::ostream &operator<<(std::ostream &os, const QuickBenchConfig &c)
{
    os << "{QuickConfig " << util::pre(c.options_) << "}";
    return os;
}

struct QuickOptions
{
    std::string name;
    std::any options;  // should be std::vector<AnyWrapper>
    bool to_df;
};

inline std::ostream &operator<<(std::ostream &os, const QuickOptions &opt)
{
    os << "{Option " << opt.name << " ";
    const auto &vec = std::any_cast<std::vector<AnyWrapper>>(opt.options);
    os << util::pre(vec) << "}";
    return os;
}

class ConfigFactory
{
public:
    using ConfigPtr = std::shared_ptr<QuickBenchConfig>;
    using It = typename std::list<QuickOptions>::iterator;

    template <typename T>
    void add_option(const std::string &name,
                    const std::initializer_list<T> &options,
                    bool to_df = true)
    {
        check_unique(name);
        std::vector<AnyWrapper> store_options;
        for (const auto &opt : options)
        {
            store_options.emplace_back(AnyWrapper{
                .value = std::make_any<T>(opt), .pre_str = util::pre_str(opt)});
        }
        records_.emplace_back(QuickOptions{
            .name = name,
            .options = std::make_any<std::vector<AnyWrapper>>(store_options),
            .to_df = to_df,
        });
        // LOG(INFO) << PRE(records_);
    }

    void add_option_df(const std::string &name,
                       const std::initializer_list<const char *> &options)
    {
        add_option<const char *>(name, options, true);
    }
    void add_option_df(const std::string &name,
                       const std::initializer_list<uint64_t> &options)
    {
        add_option<uint64_t>(name, options, true);
    }

    void configure_thread_nr(const std::initializer_list<size_t> &options)
    {
        add_option<size_t>("thread_nr", options, false /* to_df */);
    }
    void configure_coro_nr(const std::initializer_list<size_t> &options)
    {
        add_option<size_t>("coro_nr", options, false /* to_df */);
    }

    auto generate_configs()
    {
        std::vector<::bench::IBenchConfig::Pointer> ret;
        auto first_c = std::make_shared<QuickBenchConfig>();
        go(records_.begin(), first_c, ret);
        return ret;
    }

    // use recursive algorithm to generate Cartesian product
    void go(It it,
            ConfigPtr c,
            std::vector<::bench::IBenchConfig::Pointer> &ret)
    {
        if (it == records_.end())
        {
            ret.push_back(c);
            return;
        }
        const auto &record = *it;
        const auto &vec =
            std::any_cast<std::vector<AnyWrapper>>(record.options);
        for (const auto &any_wrap : vec)
        {
            auto new_c = c->clone();
            new_c->put(record.name, any_wrap, record.to_df);

            auto it2 = it;
            it2++;
            go(it2, new_c, ret);
        }
    }

private:
    void check_unique(const std::string &name)
    {
        for (const auto &record : records_)
        {
            if (record.name == name)
            {
                LOG(FATAL) << "** " << PRE(name)
                           << " already registered. Previous: " << record;
            }
        }
    }
    std::list<QuickOptions> records_;
};
}  // namespace bench
