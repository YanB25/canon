#pragma once
#include <limits>

#include "DSM.h"
#include "GlobalAddress.h"
#include "avis/avis.h"
#include "avis/provider.h"
#include "bench/request.h"
#include "thirdparty/racehashing/hashtable.h"
#include "thirdparty/racehashing/rhh_conf.h"
#include "thirdparty/racehashing/utils.h"
#include "twitter_cache/parser.h"
#include "util/RetCode.h"

namespace bench
{
class TwitterRequestGenerator
{
public:
    struct Config
    {
        std::filesystem::path base_dir;
        std::string cluster;
        uint32_t client_id;
        bool insert_only;
        std::optional<uint64_t> limit{std::numeric_limits<uint64_t>::max()};
    };
    TwitterRequestGenerator(const std::string filename) : file_name_(filename)
    {
        open();
    }
    TwitterRequestGenerator(const Config &config)
    {
        file_name_ = config.base_dir / ("cluster" + config.cluster + ".sort-" +
                                        std::to_string(config.client_id));
        insert_only_ = config.insert_only;
        open();
    }
    size_t total_nr() const
    {
        return parser_->total_nr();
    }
    size_t remain_nr() const
    {
        return parser_->remain_nr();
    }
    std::optional<twitter::Record> next() const
    {
        while (true)
        {
            auto ret = parser_->next();
            if (unlikely(!ret))
            {
                return std::nullopt;
            }
            if (insert_only_ && !ret->is_set)
            {
                continue;
            }
            return ret;
        }
    }

private:
    void open()
    {
        parser_ = std::make_shared<twitter::Parser>(file_name_);
    }
    std::string file_name_;
    std::shared_ptr<twitter::Parser> parser_;
    bool insert_only_{false};
};

inline std::ostream &operator<<(std::ostream &os,
                                const TwitterRequestGenerator::Config &c)
{
    os << "{TwitterConfig " << c.cluster << "-" << std::to_string(c.client_id)
       << "}";
    return os;
}

template <size_t kE, size_t kB, size_t kS>
class TwitterHashClient
{
public:
    constexpr static bool kReport = false;

    using RaceHashingT = patronus::hash::RaceHashing<kE, kB, kS>;
    using HandleT = typename RaceHashingT::Handle;
    using Provider = avis::BuddyProvider;
    using BufferView = patronus::hash::BufferView;
    using RaceHashingConfig = patronus::hash::RaceHashingConfig;
    using RaceHashingConfigFactory = patronus::hash::RaceHashingConfigFactory;

    struct BenchConfig
    {
        TwitterRequestGenerator g;
        // NOTE: this may have high overhead
        // dont use in benchmark
        bool en_validate{false};
    };

    TwitterHashClient(DSM::pointer dsm,
                      std::shared_ptr<avis::AvisAdaptor> avis_adaptor_,
                      GlobalAddress race_meta,
                      const BenchConfig &config,
                      size_t server_nid,
                      CoroContext *ctx)
        : dsm_(dsm),
          avis_adpt_(avis_adaptor_),
          meta_(race_meta),
          config_(config),
          server_nid_(server_nid),
          ctx_(ctx)
    {
        init_race();
        CHECK(!meta_.is_null());
    }
    size_t total_nr() const
    {
        return config_.g.total_nr();
    }
    size_t remain_nr() const
    {
        return config_.g.remain_nr();
    }

    RetCode once()
    {
        auto next = config_.g.next();
        if (unlikely(!next.has_value()))
        {
            return RC::kExit;
        }

        if (next->is_set)
        {
            // put
            m_.put_nr++;

            auto view =
                race_handle_->prepare_kv(next->key_size, next->value_size);
            next->fill_key(view.key_view().data(), view.key_view().size());
            next->fill_value(view.value_view().data(),
                             view.value_view().size());

            LOG_IF(INFO, kReport) << "[hash-client] put " << *next;
            auto rc = race_handle_->put();
            LOG_IF(INFO, kReport) << "[hash-client] put: " << rc;
            if (rc == RC::kOk)
            {
                m_.put_ok++;
            }
            validate_put(
                rc, view.key_view().to_sv(), view.value_view().to_sv());

            return rc;
        }
        else
        {
            // get
            m_.get_nr++;

            DCHECK_EQ(next->value_size, 0);
            auto view =
                race_handle_->prepare_kv(next->key_size, next->value_size);
            next->fill_key(view.key_view().data(), view.key_view().size());

            BufferView got_value;
            LOG_IF(INFO, kReport) << "[hash-client] get " << *next;
            auto rc = race_handle_->get(got_value);
            LOG_IF(INFO, kReport) << "[hash-client] get: " << rc;
            validate_get(rc, next->key, got_value.to_sv());
            if (rc == RC::kOk)
            {
                m_.get_hit++;
            }
            else if (rc == RC::kNotFound)
            {
                m_.get_miss++;
            }
            return rc;
        }
    }

    auto metric() const
    {
        return m_;
    }

    auto get_adaptor() const
    {
        return avis_adpt_;
    }
    auto get_handle() const
    {
        return avis_adpt_->get_handle();
    }

private:
    DSM::pointer dsm_;
    // std::vector<Provider> providers_;
    // avis
    avis::AvisAdaptor::Pointer avis_adpt_;
    GlobalAddress meta_;
    // GlobalAddress pub_meta_;
    // size_t pub_size_;
    BenchConfig config_;
    size_t server_nid_;
    CoroContext *ctx_;

    std::map<std::string, std::string> map_;

    // race
    std::shared_ptr<HandleT> race_handle_;

    struct OpMetric m_;

    void validate_put(RetCode rc, std::string_view key, std::string_view value)
    {
        DCHECK(rc == RC::kOk || rc == RC::kNoMem || rc == RC::kRetry)
            << PRE(rc);
        if (unlikely(config_.en_validate))
        {
            if (rc == RC::kOk)
            {
                map_[std::string(key)] = std::string(value);
            }
        }
    }

    void validate_get(RetCode rc,
                      std::string_view key_sv,
                      std::string_view got_value_sv)
    {
        if (unlikely(config_.en_validate))
        {
            auto key = std::string(key_sv);
            auto got_v = std::string(got_value_sv);
            if (rc == RC::kOk)
            {
                auto it = map_.find(key);
                if (it == map_.end())
                {
                    LOG(ERROR) << "** Expected key not found: " << key;
                }
                else
                {
                    const auto &expect_value = it->second;
                    LOG_IF(ERROR, got_v != expect_value)
                        << "** Value mismatch for key: " << key;
                }
            }
            else if (rc == RC::kNotFound)
            {
                auto it = map_.find(key);
                if (it != map_.end())
                {
                    LOG(ERROR)
                        << "** Expect found key: " << key << " but not found";
                }
            }
        }
    }
    void validate_del(RetCode rc, std::string_view key_v)
    {
        if (unlikely(config_.en_validate))
        {
            auto key = std::string(key_v);
            auto it = map_.find(key);
            if (it != map_.end())
            {
                LOG_IF(ERROR, rc != RC::kOk)
                    << "** del key " << key << " expect to succeeded. "
                    << PRE(rc);
                map_.erase(it);
            }
            else
            {
                LOG_IF(ERROR, rc != RC::kNotFound)
                    << "** del key " << key << " expect to found no key. "
                    << PRE(rc);
            }
        }
    }

    // void init_avis()
    // {
    //     auto ptl = std::make_shared<avis::PTL>(dsm_, server_nid_, ctx_);
    //     auto pub = std::make_shared<avis::Publisher<avis::BitmapPub>>(
    //         dsm_, pub_meta_, pub_size_, ctx_);
    //     avis_handle_ = std::make_shared<avis::AvisHandle>(
    //         providers_, dsm_, ptl, pub, ctx_);
    //     avis_adpt_ = avis::AvisAdaptor::make_ptr(avis_handle_, server_nid_);
    // }

    void init_race()
    {
        auto handle_conf = RaceHashingConfigFactory::get_unprotected(
            "test", false /* force match */);
        race_handle_ = HandleT::new_instance(
            meta_, handle_conf, false /* auto expand */, avis_adpt_);
    }
};
}  // namespace bench