#pragma once
#include <limits>

#include "DSM.h"
#include "GlobalAddress.h"
#include "avis/avis.h"
#include "avis/handle.h"
#include "avis/provider.h"
#include "avis/publisher.h"
#include "bench/request.h"
#include "thirdparty/racehashing/hashtable.h"
#include "thirdparty/racehashing/race.h"
#include "thirdparty/racehashing/rhh_conf.h"
#include "thirdparty/racehashing/utils.h"
#include "util/RetCode.h"

// using namespace patronus::hash;

template <size_t kE, size_t kB, size_t kS>
class HashTable
{
public:
    using Provider = avis::BuddyProvider;
    using RaceHashingT = patronus::hash::RaceHashing<kE, kB, kS>;
    using HandleT = typename RaceHashingT::Handle;
    using RaceHashingConfig = patronus::hash::RaceHashingConfig;

    HashTable(DSM::pointer dsm, size_t buddy_nr) : dsm_(dsm)
    {
        RaceHashingConfig race_conf;
        race_conf.initial_subtable = kE;
        race_conf.g_kvblock_pool_addr = (char *) dsm_->get_base_addr();
        race_conf.g_kvblock_pool_size = std::numeric_limits<size_t>::max();
        auto server_adaptor = avis::AvisAdaptor::make_ptr(dsm_);
        auto server_allocator = server_adaptor->get_manager_allocator();
        table_ = RaceHashingT::new_instance(
            server_adaptor, server_allocator, race_conf);
        dsm_->put("meta", table_->meta_raddr(), 100ms);

        uint32_t nid = dsm->get_node_id();
        auto data_size = 2_GB;
        auto total_data_size = data_size * buddy_nr;
        LOG(INFO) << "[table] trying to allocate " << total_data_size
                  << " bytes (" << util::pre_byte(total_data_size) << ")";
        auto data = dsm_->alloc_from(total_data_size, nid, 4_KB);
        CHECK(!data.is_null())
            << "Run out of memory for buddy_nr: " << buddy_nr;
        for (size_t i = 0; i < buddy_nr; ++i)
        {
            // This is HashTable, so allocate DSM from me.
            auto meta_size = 2_MB;
            auto meta = dsm_->alloc_from(meta_size, nid, 4_KB);
            CHECK(!meta.is_null())
                << "Run out of memory at " << PRE(i) << " / " << buddy_nr;
            // auto data_size = 2_GB;
            // auto data = dsm_->alloc_from(data_size, nid, 4_KB);
            // CHECK(!data.is_null())
            //     << "Run out of memory at " << PRE(i) << " / " << buddy_nr;
            auto cur_data = data + i * data_size;
            providers_.push_back(Provider{.node_id = nid,
                                          .meta_raddr = meta,
                                          .meta_size = 2_MB,
                                          .buf_raddr = cur_data,
                                          .buf_size = 2_GB,
                                          .page_size = 4_KB});
        }
        dsm_->put("size", (uint32_t) buddy_nr, 100ms);
        dsm_->put("providers",
                  providers_.data(),
                  providers_.size() * sizeof(Provider),
                  100ms);

        auto pub_size = 2_MB;
        auto pub_meta = dsm_->alloc_from(pub_size, nid);
        auto rdma_buf = dsm_->get_rdma_buffer(pub_size);
        memset(rdma_buf.buffer, 0, pub_size);
        dsm_->prepare_write(
            rdma_buf.buffer, pub_meta, pub_size, false, nullptr);
        dsm_->commit();
        dsm_->put_rdma_buffer(std::move(rdma_buf));
        dsm_->put("pub", pub_meta, 100ms);
        dsm_->put("pub_size", pub_size, 100ms);
    }
    auto meta() const
    {
        return table_->meta_raddr();
    }

    auto providers() const
    {
        return providers_;
    }
    auto table()
    {
        return table_;
    }
    ~HashTable()
    {
        for (const auto &p : providers_)
        {
            dsm_->free(p.meta_raddr, p.meta_size);
            dsm_->free(p.buf_raddr, p.buf_size);
        }
    }

private:
    DSM::pointer dsm_;
    std::vector<Provider> providers_;

    std::shared_ptr<RaceHashingT> table_;
};

template <size_t kE, size_t kB, size_t kS>
class HashClient
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
        std::shared_ptr<bench::IRequestGenerator> g;
        // NOTE: this may have high overhead
        // dont use in benchmark
        bool en_validate{false};
    };

    HashClient(DSM::pointer dsm,
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

    RetCode put(std::string_view key, std::string_view value)
    {
        m_.put_nr++;

        auto key_size = key.size();
        size_t value_size = value.size();
        auto view = race_handle_->prepare_kv(key_size, value_size);
        view.fill_key_value(key, value);
        auto rc = race_handle_->put();
        if (rc == RC::kOk)
        {
            m_.put_ok++;
        }

        validate_put(rc, key, value);

        return rc;
    }

    RetCode random_put()
    {
        m_.put_nr++;

        auto view = race_handle_->prepare_kv(key_size(), value_size());
        fill_key(view.key_view());
        fill_value(view.value_view());

        LOG_IF(INFO, kReport) << "[hash-client] random_put";
        auto rc = race_handle_->put();
        LOG_IF(INFO, kReport) << "[hash-client] random_put: " << rc;
        if (rc == RC::kOk)
        {
            m_.put_ok++;
        }
        validate_put(rc, view.key_view().to_sv(), view.value_view().to_sv());

        return rc;
    }

    std::pair<RetCode, std::string> get(std::string_view key)
    {
        m_.get_nr++;

        auto view = race_handle_->prepare_kv(key.size(), 0);
        view.fill_key(key);

        BufferView got_value;
        auto rc = race_handle_->get(got_value);
        validate_get(rc, key, got_value.to_sv());
        if (rc == RC::kOk)
        {
            m_.get_hit++;
        }
        else if (rc == RC::kNotFound)
        {
            m_.get_miss++;
        }
        if (rc == RC::kOk)
        {
            return {rc, std::string(got_value.to_sv())};
        }
        return {rc, ""};
    }

    RetCode random_get()
    {
        m_.get_nr++;

        auto view = race_handle_->prepare_kv(key_size(), 0);
        fill_key(view.key_view());
        BufferView got_value;
        LOG_IF(INFO, kReport) << "[hash-client] random_get";
        auto rc = race_handle_->get(got_value);
        LOG_IF(INFO, kReport) << "[hash-client] random_get: " << rc;
        if (rc == RC::kOk)
        {
            m_.get_hit++;
        }
        else if (rc == RC::kNotFound)
        {
            m_.get_miss++;
        }

        validate_get(rc, view.key_view().to_sv(), got_value.to_sv());
        return rc;
    }
    RetCode del(std::string_view key)
    {
        m_.del_nr++;

        auto view = race_handle_->prepare_kv(key.size(), 0);
        view.fill_key(key);

        auto rc = race_handle_->del();

        if (rc == RC::kOk)
        {
            m_.del_hit++;
        }
        else if (rc == RC::kNotFound)
        {
            m_.del_miss++;
        }
        validate_del(rc, key);

        return rc;
    }
    auto metric() const
    {
        return m_;
    }

    RetCode random_del()
    {
        m_.del_nr++;
        auto view = race_handle_->prepare_kv(key_size(), 0);
        fill_key(view.key_view());

        LOG_IF(INFO, kReport) << "[hash-client] random_del";
        auto rc = race_handle_->del();
        LOG_IF(INFO, kReport) << "[hash-client] random_del: " << rc;
        if (rc == RC::kOk)
        {
            m_.del_hit++;
        }
        else if (rc == RC::kNotFound)
        {
            m_.del_miss++;
        }
        else if (rc == RC::kNotFound)
        {
            m_.del_miss++;
        }
        validate_del(rc, view.key_view().to_sv());

        return rc;
    }
    void reset()
    {
        auto m2 = map_;
        for (const auto &[key, value] : m2)
        {
            CHECK_EQ(del(key), RC::kOk) << "** failed to del key: " << key;
        }
        map_.clear();
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

    size_t key_size() const
    {
        return config_.g->next_key_size();
    }
    size_t value_size() const
    {
        return config_.g->next_value_size();
    }
    void fill_key(BufferView buf)
    {
        config_.g->fill_key(buf.data(), buf.size());
    }
    void fill_value(BufferView buf)
    {
        config_.g->fill_value(buf.data(), buf.size());
    }

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