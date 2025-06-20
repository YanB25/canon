#pragma once
#include <cinttypes>
#include <cstddef>
#include <cstring>

#include "./hashtable_handle_impl.h"
#include "./utils.h"
#include "Common.h"
#include "avis/mm.h"
#include "patronus/RdmaAdaptor.h"
#include "patronus/memory/patronus_wrapper_allocator.h"
#include "thirdparty/racehashing/kv_block.h"
#include "util/Hexdump.hpp"
#include "util/Pre.h"
#include "util/TimeConv.h"
#include "util/Tracer.h"

using namespace util::literals;
namespace patronus::hash
{
class IHashtable
{
public:
    virtual void init(util::TraceView trace = util::nulltrace) = 0;
    virtual void deinit(util::TraceView trace = util::nulltrace) = 0;
    virtual KVBlockView prepare_kv(size_t key_len, size_t value_len) = 0;
    /**
     * @brief put the key-value pair to the hashtable.
     * auto view = handle->prepare_kv(key_len, value_len);
     * view.fill_key_value(key, value);
     * auto rc = handle->put();
     * CHECK(rc == RC::kOk || rc == RC::kNoMem);
     */
    virtual RetCode put(util::TraceView trace = util::nulltrace) = 0;
    /**
     * @brief get the value from the hashtable.
     * auto view = handle->prepare_kv(key_len, 0);
     * view.fill_key(key);
     * BufferView view;
     * auto rc = handle->get();
     * CHECK(rc == RC::kOk || rjc == RC::kNotFound);
     */
    virtual RetCode get(Value &value,
                        util::TraceView trace = util::nulltrace) = 0;
    /**
     * @brief get the value from the hashtable.
     * auto view = handle->prepare_kv(key_len, 0);
     * view.fill_key(key);
     * BufferView view;
     * auto rc = handle->del();
     * CHECK(rc == RC::kOk || rc == RC::kNotFound);
     */
    virtual RetCode del(util::TraceView trace = util::nulltrace) = 0;

    virtual ~IHashtable() = default;
};

template <size_t kDEntryNr, size_t kBucketGroupNr, size_t kSlotNr>
class RaceHashingHandleWrapperImpl : public IHashtable
{
public:
    using pointer = std::shared_ptr<RaceHashingHandleWrapperImpl>;
    using RaceHashingHandleT =
        RaceHashingHandleImpl<kDEntryNr, kBucketGroupNr, kSlotNr>;
    RaceHashingHandleWrapperImpl(GlobalAddress table_meta_addr,
                                 const RaceHashingHandleConfig &conf,
                                 bool auto_expand,
                                 IRdmaAdaptor::pointer rdma_adpt)
        : rhh_(table_meta_addr, conf, auto_expand, rdma_adpt),
          rdma_adpt_(rdma_adpt)
    {
    }
    auto *get_adaptor()
    {
        return rdma_adpt_.get();
    }
    static pointer new_instance(GlobalAddress table_meta_addr,
                                const RaceHashingHandleConfig &conf,
                                bool auto_expand,
                                IRdmaAdaptor::pointer rdma_adpt)
    {
        return std::make_shared<RaceHashingHandleWrapperImpl>(
            table_meta_addr, conf, auto_expand, rdma_adpt);
    }
    static size_t max_capacity()
    {
        return RaceHashingHandleT::max_capacity();
    }

    void init(util::TraceView trace = util::nulltrace) override
    {
        rhh_.init(trace);
        rdma_adpt_->put_all_rdma_buffer();
    }
    void deinit(util::TraceView trace = util::nulltrace) override
    {
        rhh_.deinit(trace);
        rdma_adpt_->put_all_rdma_buffer();
    }

    KVBlockView prepare_kv(size_t key_len, size_t value_len) override
    {
        // clear contexts
        rdma_adpt_->put_all_rdma_buffer();

        key_len_ = key_len;
        value_len_ = value_len;
        auto kvblock_size = sizeof(KVBlock) + key_len + value_len;

        // a) In the RACE hashing implementation, the @kvblock_size is stored
        // in the unit of @kLenUnit (actual_*)
        auto [actual_kvblock_size, _] =
            get_actual_kvblock_tagged_size(kvblock_size);
        rdma_buffer_ = rdma_adpt_->get_rdma_buffer(actual_kvblock_size);

        KVBlockView view(rdma_buffer_.buffer, key_len, value_len);
        *view.key_len_buf() = key_len;
        *view.value_len_buf() = value_len;
        return view;
    }

    RetCode put(util::TraceView trace = util::nulltrace) override
    {
        maybe_start_put_trace();

        auto kvblock_view =
            KVBlockView(rdma_buffer_.buffer, key_len_, value_len_);
        auto rc = rhh_.put(kvblock_view, trace);

        maybe_end_trace(rc);
        return rc;
    }

    RetCode del(util::TraceView trace = util::nulltrace) override
    {
        maybe_start_del_trace();

        auto kvblock_view =
            KVBlockView(rdma_buffer_.buffer, key_len_, value_len_);

        auto rc = rhh_.del(kvblock_view.key(), trace);

        maybe_end_trace(rc);
        return rc;
    }
    RetCode get(BufferView &value,
                util::TraceView trace = util::nulltrace) override
    {
        maybe_start_get_trace();

        auto kvblock_view =
            KVBlockView(rdma_buffer_.buffer, key_len_, value_len_);

        auto rc = rhh_.get(kvblock_view.key(), value, trace);

        maybe_end_trace(rc);
        return rc;
    }
    RetCode expand(size_t subtable_idx, util::TraceView trace)
    {
        maybe_start_expand_trace();

        DLOG_IF(INFO, config::kMonitorRdma) << "[race] Started to Expand";
        auto rc = rhh_.expand(subtable_idx, trace);
        DLOG_IF(INFO, config::kMonitorRdma)
            << "[race] expand subtable[" << subtable_idx << "] "
            << pre_rdma_adaptor(rdma_adpt_);

        rdma_adpt_->put_all_rdma_buffer();

        maybe_end_trace(rc);
        return rc;
    }
    void debug_out(std::ostream &os) const
    {
        return rhh_.debug_out(os);
    }

    template <size_t kA, size_t kB, size_t kC>
    friend std::ostream &operator<<(
        std::ostream &os, const RaceHashingHandleWrapperImpl<kA, kB, kC> &rhh);

    void hack_trigger_rdma_protection_error()
    {
        rhh_.hack_trigger_rdma_protection_error();
    }

    RetCode update_directory_cache(util::TraceView trace)
    {
        rhh_.update_directory_cache(trace);
    }
    ~RaceHashingHandleWrapperImpl()
    {
    }

private:
    RaceHashingHandleT rhh_;
    IRdmaAdaptor::pointer rdma_adpt_;

    Buffer rdma_buffer_{};
    size_t key_len_{};
    size_t value_len_{};

    void maybe_start_get_trace()
    {
        return maybe_do_start_trace("get", ::config::kRdmaTraceRateGet);
    }
    void maybe_start_put_trace()
    {
        return maybe_do_start_trace("put", ::config::kRdmaTraceRatePut);
    }
    void maybe_start_del_trace()
    {
        return maybe_do_start_trace("del", ::config::kRdmaTraceRateDel);
    }
    void maybe_start_expand_trace()
    {
        return maybe_do_start_trace("expand", ::config::kRdmaTraceRateExpand);
    }
    void maybe_do_start_trace(const char *name, double prob)
    {
        if constexpr (::config::kEnableRdmaTrace)
        {
            if (true_with_prob(prob))
            {
                rdma_adpt_->enable_trace(name);
                rdma_adpt_->trace_pin(name);
            }
        }
    }
    void maybe_end_trace(RetCode rc)
    {
        if constexpr (::config::kEnableRdmaTrace)
        {
            if (unlikely(rdma_adpt_->trace_enabled()))
            {
                rdma_adpt_->trace_pin("finished");
                rdma_adpt_->end_trace(nullptr /* give u nothing */);
                LOG(INFO) << "[trace] result: " << rc << ", "
                          << pre_rdma_adaptor_trace(rdma_adpt_);
            }
        }
    }
};

template <size_t kDEntryNr, size_t kBucketGroupNr, size_t kSlotNr>
inline std::ostream &operator<<(
    std::ostream &os,
    const RaceHashingHandleWrapperImpl<kDEntryNr, kBucketGroupNr, kSlotNr>
        &rhh_wrap)
{
    os << "{" << rhh_wrap.rhh_ << ", " << rhh_wrap.rdma_adpt_ << "}";
    return os;
}

template <size_t kD, size_t kB, size_t kS>
struct pre_rhh_debug
{
    RaceHashingHandleWrapperImpl<kD, kB, kS> *rhh;
};
template <size_t kD, size_t kB, size_t kS>
inline std::ostream &operator<<(std::ostream &os, pre_rhh_debug<kD, kB, kS> rhh)
{
    CHECK_NOTNULL(rhh.rhh)->debug_out(os);
    return os;
}

}  // namespace patronus::hash
