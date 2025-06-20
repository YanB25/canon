#pragma once

#include <cinttypes>
#include <cstddef>

#include "./hashtable.h"
#include "./mock_rdma_adaptor.h"
#include "./rhh_conf.h"
#include "./utils.h"
#include "avis/debug.h"
#include "thirdparty/racehashing/debug.h"
#include "thirdparty/racehashing/kv_block.h"
#include "thirdparty/racehashing/slot.h"
#include "util/Hexdump.hpp"
#include "util/IRdmaAdaptor.h"
#include "util/Likely.h"
#include "util/RetCode.h"
#include "util/Tracer.h"

namespace patronus::hash
{
struct Location
{
    Location(const SlotHandle &s, const KVBlockHandle &k)
        : slot_handle(s), kvblock_handle(k)
    {
    }

    SlotHandle slot_handle;
    // kvblock_handle are just *attached* data
    // not included in hash(...) and operator<=>(...)
    KVBlockHandle kvblock_handle;

    auto operator<=>(const Location &rhs) const
    {
        if (debug())
        {
            if (slot_handle == rhs.slot_handle)
            {
                CHECK_EQ(kvblock_handle, rhs.kvblock_handle)
                    << "** possibily internal inconsistency.";
            }
        }
        return slot_handle <=> rhs.slot_handle;
    }
    bool operator==(const Location &rhs) const
    {
        return slot_handle == rhs.slot_handle;
    }
};
}  // namespace patronus::hash
namespace std
{
template <>
struct hash<patronus::hash::Location>
{
    std::size_t operator()(const patronus::hash::Location &loc) const
    {
        return std::hash<patronus::hash::SlotHandle>{}(loc.slot_handle);
    }
};

}  // namespace std

namespace patronus::hash
{
/**
 * @brief HashTableHandle is a handler for each client. Containing the cache of
 * GD and directory, and how to find the server-side hash table.
 *
 */
template <size_t kDEntryNr, size_t kBucketGroupNr, size_t kSlotNr>
class RaceHashingHandleImpl
{
public:
    using SubTableHandleT = SubTableHandle<kBucketGroupNr, kSlotNr>;
    using SubTableT = SubTable<kBucketGroupNr, kSlotNr>;
    using RaceHashingT = RaceHashing<kDEntryNr, kBucketGroupNr, kSlotNr>;
    using pointer = std::shared_ptr<RaceHashingHandleImpl>;

    using MetaT = RaceHashingMeta<kDEntryNr, kBucketGroupNr, kSlotNr>;

    constexpr static size_t kOngoingHandleSize = 32;

    constexpr static size_t V = ::config::verbose::kUserApp_2;

    RaceHashingHandleImpl(GlobalAddress table_meta_addr,
                          const RaceHashingHandleConfig &conf,
                          bool auto_expand,
                          IRdmaAdaptor::pointer rdma_adpt)
        : table_meta_addr_(table_meta_addr),
          conf_(conf),
          auto_expand_(auto_expand),
          auto_update_dir_(auto_expand),
          rdma_adpt_(rdma_adpt)
    {
        read_kvblock_handles_.reserve(kOngoingHandleSize);
    }
    ~RaceHashingHandleImpl()
    {
        deinit();
    }
    constexpr static size_t meta_size()
    {
        return RaceHashingT::meta_size();
    }

    void init(util::TraceView trace = util::nulltrace);
    void deinit(util::TraceView trace = util::nulltrace);
    /**
     * @key: DMA-able buffer for the key.
     */
    RetCode del(const BufferView &key, util::TraceView trace = util::nulltrace);
    RetCode put(KVBlockView &kvblock_view,
                util::TraceView trace = util::nulltrace);
    RetCode get(const BufferView &key,  // in
                BufferView &value,      // out
                util::TraceView trace = util::nulltrace);

    void hack_trigger_rdma_protection_error();

    static size_t max_capacity()
    {
        return RaceHashingT::max_capacity();
    }

    size_t gd() const
    {
        return cached_meta_.gd;
    }

    template <size_t kA, size_t kB, size_t kC>
    friend std::ostream &operator<<(
        std::ostream &os, const RaceHashingHandleImpl<kA, kB, kC> &rhh);

    constexpr static size_t subtable_nr()
    {
        return kDEntryNr;
    }

private:
    // the address of hash table at remote side
    GlobalAddress table_meta_addr_;
    MetaT cached_meta_;
    RaceHashingHandleConfig conf_;
    bool auto_expand_;
    bool auto_update_dir_;
    IRdmaAdaptor::pointer rdma_adpt_;
    std::array<RemoteMemHandle, subtable_nr()> subtable_mem_handles_{};
    RemoteMemHandle directory_mem_handle_;

    RemoteMemHandle fake_handle_;

    bool inited_{false};

    // for client private kvblock memory
    GlobalAddress kvblock_pool_raddr_;

    // KVBlockView get_kvblock_view()
    // {
    //     return KVBlockView(usr_ctx_.rdma_buffer_.buffer,
    //                        usr_ctx_.key_len_,
    //                        usr_ctx_.value_len_);
    // }

    RetCode update_directory_cache(util::TraceView trace);

    void eager_init_subtable_mem_handle()
    {
        for (size_t i = 0; i < cached_subtable_nr(); ++i)
        {
            // trigger binding
            std::ignore = get_subtable_mem_handle(i);
        }
    }

    RetCode remove_if_exists(size_t subtable_idx,
                             const std::unordered_set<SlotHandle> &slot_handles,
                             const Key &key,
                             util::TraceView trace);
    GlobalAddress to_insert_block_raddr() const
    {
        DCHECK(to_insert_block.raddr_.has_value());
        return *to_insert_block.raddr_;
    }
    RemoteMemHandle &to_insert_block_handle()
    {
        return to_insert_block.handle_;
    }
    size_t to_insert_block_kvblock_size()
    {
        DCHECK_GT(to_insert_block.kvblock_size_, 0);
        return to_insert_block.kvblock_size_;
    }
    size_t to_insert_block_tagged_len()
    {
        DCHECK_GT(to_insert_block.tagged_len_, 0);
        return to_insert_block.tagged_len_;
    }

    RetCode do_put_once(const Key &key,
                        uint64_t hash,
                        util::TraceView trace = util::nulltrace);
    RetCode do_put_loop(const Key &key, uint64_t hash, util::TraceView trace);
    std::array<RemoteMemHandle, subtable_nr()> expand_lock_handles_{};
    std::chrono::time_point<std::chrono::steady_clock> lock_handle_last_extend_;
    bool maybe_expand_try_extend_lock_lease(
        size_t subtable_idx, [[maybe_unused]] util::TraceView trace);
    RetCode expand_try_lock_subtable_drain(size_t subtable_idx);
    GlobalAddress lock_remote_addr(size_t subtable_idx) const
    {
        DCHECK_LT(subtable_idx, kDEntryNr);
        auto offset = offsetof(MetaT, expanding);
        offset += subtable_idx * sizeof(uint64_t);
        return table_meta_addr_ + offset;
    }
    RetCode expand_unlock_subtable_nodrain(size_t subtable_idx)
    {
        const auto &c = conf_.expand;
        if (c.use_patronus_lock)
        {
            rdma_adpt_->relinquish_perm(
                expand_lock_handles_[subtable_idx], 0, c.patronus_unlock_flag);
            return kOk;
        }
        else
        {
            auto rdma_buf = rdma_adpt_->get_rdma_buffer(8);
            DCHECK_GE(rdma_buf.size, 8);
            auto remote = lock_remote_addr(subtable_idx);
            *(uint64_t *) DCHECK_NOTNULL(rdma_buf.buffer) = 0;  // no lock
            auto &dir_mem_handle = get_directory_mem_handle();
            return rdma_adpt_->rdma_write(remote,
                                          (char *) rdma_buf.buffer,
                                          8,
                                          0 /* flag */,
                                          dir_mem_handle);
        }
    }
    RetCode expand_write_entry_nodrain(size_t subtable_idx,
                                       GlobalAddress subtable_remote_addr)
    {
        auto entry_size = sizeof(subtable_remote_addr);
        auto rdma_buf = rdma_adpt_->get_rdma_buffer(entry_size);
        DCHECK_GE(rdma_buf.size, entry_size);

        *(uint64_t *) rdma_buf.buffer = subtable_remote_addr.val;
        auto remote = entries_remote_addr(subtable_idx);
        auto &dir_mem_handle = get_directory_mem_handle();
        return rdma_adpt_->rdma_write(remote,
                                      (char *) rdma_buf.buffer,
                                      entry_size,
                                      0 /* flag */,
                                      dir_mem_handle);
    }
    GlobalAddress entries_remote_addr(size_t subtable_idx) const
    {
        DCHECK_LT(subtable_idx, kDEntryNr)
            << "make no sense to ask for overflowed addr";
        auto offset = offsetof(MetaT, entries);
        offset += subtable_idx * sizeof(SubTableT *);
        return table_meta_addr_ + offset;
    }
    GlobalAddress ld_remote_addr(size_t subtable_idx) const
    {
        DCHECK_LT(subtable_idx, kDEntryNr);
        auto offset = offsetof(MetaT, lds);
        offset += subtable_idx * sizeof(uint32_t);
        return table_meta_addr_ + offset;
    }
    RetCode expand_update_ld_nodrain(size_t subtable_idx, uint32_t ld)
    {
        // just uint32_t
        // I am afraid that I will change the type of ld and ruins everything
        DCHECK_LT(subtable_idx, kDEntryNr);
        using ld_t =
            typename std::remove_reference<decltype(cached_meta_.lds[0])>::type;
        auto entry_size = sizeof(ld_t);
        auto rdma_buf = rdma_adpt_->get_rdma_buffer(entry_size);
        DCHECK_GE(rdma_buf.size, entry_size);
        *(ld_t *) rdma_buf.buffer = ld;
        auto remote = ld_remote_addr(subtable_idx);
        auto &dir_mem_handle = get_directory_mem_handle();
        return rdma_adpt_->rdma_write(remote,
                                      (char *) rdma_buf.buffer,
                                      entry_size,
                                      0 /* flag */,
                                      dir_mem_handle);
    }
    GlobalAddress gd_remote_addr() const
    {
        return table_meta_addr_ + offsetof(MetaT, gd);
    }
    RetCode expand_cas_gd_drain(uint64_t expect, uint64_t desired)
    {
        auto rdma_buf = rdma_adpt_->get_rdma_buffer(8);
        DCHECK_GE(rdma_buf.size, 8);
        auto remote = gd_remote_addr();
        auto &dir_mem_handle = get_directory_mem_handle();
        rdma_adpt_
            ->rdma_cas(remote,
                       expect,
                       desired,
                       rdma_buf.buffer,
                       0 /* flag */,
                       dir_mem_handle)
            .expect(RC::kOk);
        auto rc = rdma_adpt_->commit();
        if (unlikely(rc == kRdmaProtectionErr))
        {
            return rc;
        }
        CHECK_EQ(rc, kOk);

        uint64_t r = *(uint64_t *) rdma_buf.buffer;
        DCHECK_LE(r, log2(kDEntryNr))
            << "The old gd should not larger than log2(entries_nr)";
        bool success = r == expect;
        if (success)
        {
            DCHECK_EQ(cached_meta_.gd, expect);
            cached_meta_.gd = desired;
            return kOk;
        }
        return kRetry;
    }
    RetCode expand_update_remote_bucket_header_drain(
        SubTableHandleT &subtable_handle,
        uint32_t ld,
        uint32_t suffix,
        util::TraceView trace)
    {
        auto rc = subtable_handle.update_bucket_header_nodrain(
            ld, suffix, *rdma_adpt_, trace);
        if (rc != kOk)
        {
            return rc;
        }
        return rdma_adpt_->commit(trace);
    }
    RetCode expand_init_and_update_remote_bucket_header_drain(
        SubTableHandleT &subtable_handle,
        uint32_t ld,
        uint32_t suffix,
        util::TraceView trace)
    {
        // just a wrapper, this parameters to the ctor is not used.
        auto rc = subtable_handle.init_and_update_bucket_header_drain(
            ld, suffix, *rdma_adpt_, trace);
        if (rc != kOk)
        {
            return rc;
        }
        return rdma_adpt_->commit(trace);
    }
    RetCode expand_install_subtable_nodrain(size_t subtable_idx,
                                            GlobalAddress new_remote_subtable)
    {
        auto size = sizeof(new_remote_subtable.val);
        auto rdma_buf = rdma_adpt_->get_rdma_buffer(size);
        DCHECK_GE(rdma_buf.size, size);
        *(uint64_t *) rdma_buf.buffer = new_remote_subtable.val;
        auto remote = entries_remote_addr(subtable_idx);
        auto &dir_mem_handle = get_directory_mem_handle();
        return rdma_adpt_->rdma_write(remote,
                                      (char *) rdma_buf.buffer,
                                      size,
                                      0 /* flag */,
                                      dir_mem_handle);
    }
    /**
     * pre-condition: the subtable of dst_staddr is all empty.
     */
    RetCode expand_migrate_subtable(SubTableHandleT &src_st_handle,
                                    SubTableHandleT &dst_st_handle,
                                    size_t bit,
                                    util::TraceView trace);
    // TODO: expand still has lots of problem
    // When trying to expand to dst subtable, the concurrent clients can insert
    // lots of items to make it full. Therefore cascaded expansion is required.
    // The client can always find out cache stale and update their directory
    // cache.
    RetCode expand(size_t subtable_idx, util::TraceView trace);
    void print_latest_meta_image(util::TraceView trace);
    RetCode expand_cascade_update_entries_drain(
        GlobalAddress next_subtable_addr,
        GlobalAddress origin_subtable_addr,
        uint32_t ld,
        uint32_t suffix,
        IRdmaAdaptor &rdma_adpt,
        RemoteMemHandle &dir_mem_handle,
        util::TraceView trace);

    RetCode phase_two_deduplicate(const Key &key,
                                  uint64_t hash,
                                  uint32_t cached_ld,
                                  ssize_t retry_nr,
                                  util::TraceView trace);

    RetCode do_remove(size_t subtable_idx,
                      SlotHandle slot_handle,
                      KVBlockHandle kvblock_handle,
                      util::TraceView trace);

    auto deterministic_choose_slot(const std::unordered_set<Location> &views)
    {
        CHECK(!views.empty());
        return std::min_element(views.begin(), views.end());
    }
    RetCode get_real_match_handles(
        const std::unordered_set<SlotHandle> &slot_handles,
        const Key &key,
        std::unordered_set<Location> &real_matches,
        util::TraceView trace)
    {
        auto f = [&real_matches](const Key &,
                                 SlotHandle slot,
                                 KVBlockHandle kvblock_handle,
                                 util::TraceView) {
            DCHECK_EQ(slot.ptr(), kvblock_handle.remote_addr())
                << "** internal inconsistency?";

            real_matches.emplace(slot, kvblock_handle);
            return kOk;
        };
        return for_the_real_match_do(slot_handles, key, std::move(f), trace);
    }
    RetCode put_phase_one(const Key &key,
                          GlobalAddress kv_block,
                          size_t len,
                          uint64_t hash,
                          util::TraceView trace);
    RetCode insert_if_exist_empty_slot(
        size_t subtable_idx,
        const TwoCombinedBucketHandle<kSlotNr> &cb,
        uint32_t ld,
        uint32_t suffix,
        SlotView new_slot,
        util::TraceView trace);
    bool has_to_insert_block() const
    {
        return to_insert_block.raddr_.has_value();
    }

    [[nodiscard]] RetCode prepare_alloc_kv(const Key &key, const Value &value)
    {
        // a) In the RACE hashing implementation, the @kvblock_size is stored
        // in the unit of @kLenUnit (actual_*)
        size_t kvblock_size = sizeof(KVBlock) + key.size() + value.size();
        auto [actual_kvblock_size, actual_ptr_len] =
            get_actual_kvblock_tagged_size(kvblock_size);

        // this ensure to_insert_block is well-formed and enough
        auto rc = to_insert_block.prepare(rdma_adpt_.get(),
                                          conf_.alloc_kvblock,
                                          actual_kvblock_size,
                                          actual_ptr_len);
        return rc;
    }
    RetCode consume_kv_block()
    {
        to_insert_block.consume(rdma_adpt_.get(), conf_.alloc_kvblock);
        return RC::kOk;
    }

    // prepare_write_kv
    // Expected:
    // The whole KVBlockView has been set up except for the hash value.
    RetCode prepare_write_kv(KVBlockView &kvblock_view,
                             GlobalAddress kv_raddr,
                             size_t kvblock_size,
                             RemoteMemHandle &kvblock_handle,
                             IRdmaAdaptor &rdma_adpt,
                             util::TraceView trace)
    {
        DCHECK(kvblock_handle.valid());

        DLOG_IF(INFO, config::kEnableDebug)
            << "[race][trace] prepare_write_kv: allocate kv_block: " << kv_raddr
            << ", allocated_size: " << kvblock_size << ". "
            << util::pre(trace.kv());

        // << std::endl;
        //   << util::Hexdump(kvblock_view.buffer(), kvblock_size);

        return rdma_adpt.rdma_write(kv_raddr,
                                    (char *) kvblock_view.buffer(),
                                    kvblock_size,
                                    0 /* flag */,
                                    kvblock_handle);
    }
    // using ApplyF = std::function<RetCode(
    //     const Key &, SlotHandle, KVBlockHandle, util::TraceView)>;

    template <typename Fn>
    RetCode for_the_real_match_do(
        const std::unordered_set<SlotHandle> &slot_handles,
        const Key &key,
        Fn &&func,
        util::TraceView trace);

    RetCode get_from_slot_views(
        const std::unordered_set<SlotHandle> &slot_handles,
        const Key &key,
        BufferView &value,
        util::TraceView trace)
    {
        bool has_found = false;
        auto f = [&value, &has_found](const Key &key,
                                      SlotHandle slot_handle,
                                      KVBlockHandle kvblock_handle,
                                      util::TraceView trace) {
            std::ignore = key;
            std::ignore = slot_handle;

            DLOG_IF(INFO, config::kEnableDebug)
                << "[race][trace] get_from_slot_views SUCC: slot_handle "
                << slot_handle << ". " << util::pre(trace.kv());

            value = BufferView(
                (char *) kvblock_handle.kv_buf() + kvblock_handle.key_len(),
                kvblock_handle.value_len());
            DCHECK(!has_found) << "** double has_found.";
            has_found = true;

            // if (trace.enabled())
            // {
            //     std::string expect_val = trace.get("expected");
            //     bool report = !value.to_sv().starts_with(expect_val);

            //     if (unlikely(report))
            //     {
            //         CHECK_EQ(slot_handle.ptr(),
            //         kvblock_handle.remote_addr()); LOG(ERROR) << "** Value
            //         mismatch detected. Expect "
            //                    << expect_val << ", got "
            //                    << value.to_sv().substr(0, expect_val.size());
            //         LOG(INFO) << "[report] slot_handle at "
            //                   << slot_handle.remote_addr()
            //                   << ", pointing at kv_block: "
            //                   << kvblock_handle.remote_addr();
            //         auto *kvblock = kvblock_handle.buffer_addr();
            //         std::string_view kvblock_key(kvblock->buf,
            //                                      kvblock->key_len);
            //         LOG(INFO) << "[report] (double check) kvblock's key: "
            //                   << kvblock_key << " vs our key " << key;
            //         std::string_view kvblock_val(
            //             kvblock->buf + kvblock->key_len, kvblock->value_len);
            //         LOG(INFO) << "[report] kvblock's val: "
            //                   << kvblock_val.substr(0, expect_val.size())
            //                   << " vs expected val: " << expect_val <<
            //                   std::endl
            //                   << util::Hexdump(kvblock, 128);
            //     }
            // }

            return kOk;
        };

        return for_the_real_match_do(slot_handles, key, f, trace);
    }

    RetCode update_if_exists(size_t subtable_idx,
                             const std::unordered_set<SlotHandle> &slot_handles,
                             const Key &key,
                             SlotView new_slot,
                             util::TraceView trace);

    RetCode is_real_match(KVBlock *kvblock,
                          const Key &key,
                          util::TraceView trace);

    RemoteMemHandle alloc_fake_handle()
    {
        auto ac_flag = (flag_t) AcquireRequestFlag::kNoRpc;
        auto ret = rdma_adpt_->acquire_perm(GlobalAddress::Null(),
                                            0,
                                            std::numeric_limits<size_t>::max(),
                                            1ns,
                                            ac_flag);
        DCHECK(ret.valid());
        return ret;
    }
    void relinquish_fake_handle(RemoteMemHandle &handle)
    {
        if (handle.valid())
        {
            auto rel_flag = (flag_t) LeaseModifyFlag::kNoRpc;
            rdma_adpt_->relinquish_perm(handle, 0, rel_flag);
        }
    }

    RemoteMemHandle remote_alloc_acquire_subtable_directory(size_t size)
    {
        auto flag = (flag_t) AcquireRequestFlag::kNoGc |
                    (flag_t) AcquireRequestFlag::kWithAllocation |
                    (flag_t) AcquireRequestFlag::kNoBindPR;
        return rdma_adpt_->acquire_perm(
            GlobalAddress::Null(), conf_.subtable_hint, size, 0ns, flag);
    }

    std::vector<RemoteMemHandle> read_kvblock_handles_;
    RemoteMemHandle kvblock_region_handle_;
    RemoteMemHandle &begin_read_kvblock(GlobalAddress raddr, size_t size)
    {
        if (conf_.kvblock_region.has_value())
        {
            return begin_read_kvblock_region(raddr, size);
        }
        else
        {
            return begin_read_kvblock_individual(raddr, size);
        }
    }
    RemoteMemHandle &begin_read_kvblock_region(
        [[maybe_unused]] GlobalAddress raddr, [[maybe_unused]] size_t size)
    {
        if (unlikely(!kvblock_region_handle_.valid()))
        {
            const auto &c = conf_.kvblock_region.value();
            kvblock_region_handle_ =
                rdma_adpt_->acquire_perm(cached_meta_.kvblock_pool_raddr,
                                         c.alloc_hint,
                                         cached_meta_.kvblock_pool_size,
                                         c.ns,
                                         c.acquire_flag);
            DCHECK(kvblock_region_handle_.valid());
        }
        return kvblock_region_handle_;
    }
    RemoteMemHandle &begin_read_kvblock_individual(GlobalAddress raddr,
                                                   size_t size)
    {
        const auto &c = conf_.read_kvblock;
        read_kvblock_handles_.emplace_back(rdma_adpt_->acquire_perm(
            raddr, c.alloc_hint, size, c.ns, c.acquire_flag));
        return read_kvblock_handles_.back();
    }
    void end_read_kvblock()
    {
        if (conf_.kvblock_region.has_value())
        {
            return end_read_kvblock_region();
        }
        else
        {
            return end_read_kvblock_individual();
        }
    }
    void end_read_kvblock_region()
    {
    }
    void end_read_kvblock_individual()
    {
        const auto &c = conf_.read_kvblock;
        for (auto &handle : read_kvblock_handles_)
        {
            rdma_adpt_->relinquish_perm(
                handle, c.alloc_hint, c.relinquish_flag);
        }
        read_kvblock_handles_.clear();
    }

    struct ToInsertBlock
    {
        std::optional<GlobalAddress> raddr_;
        RemoteMemHandle handle_;
        size_t kvblock_size_;
        uint8_t tagged_len_;
        void consume(IRdmaAdaptor *adpt, const MemHandleDecision &c)
        {
            // NOTE: don't do free(raddr_) here
            // the memory is *used*, not *freed*.
            raddr_.reset();
            if (handle_.valid())
            {
                adpt->relinquish_perm(handle_, c.alloc_hint, c.relinquish_flag);
                DCHECK(!handle_.valid());
            }
            tagged_len_ = 0;
            kvblock_size_ = 0;
        }
        [[nodiscard]] RetCode prepare(IRdmaAdaptor *adpt,
                                      const MemHandleDecision &c,
                                      size_t kvblock_size,
                                      size_t tagged_len)
        {
            // check whether we need (re)-alloc
            // NOTE: we do re-allocation when size mismatch:
            // we assume a high-performance allocator here (with caches)
            if (!raddr_.has_value() || kvblock_size_ != kvblock_size)
            {
                if (raddr_.has_value())
                {
                    adpt->remote_free(*raddr_, kvblock_size_, 0);
                }
                if (handle_.valid())
                {
                    adpt->relinquish_perm(
                        handle_, c.alloc_hint, c.relinquish_flag);
                }
                kvblock_size_ = 0;
                tagged_len_ = 0;
                raddr_ = adpt->remote_alloc(kvblock_size, 0);
                if (unlikely(raddr_->is_null()))
                {
                    // LOG(WARNING)
                    //     << "[table] run out of remote memory: remote_alloc("
                    //     << kvblock_size << ") -> nullptr";
                    return RC::kNoMem;
                }
                handle_ = adpt->acquire_perm(GlobalAddress::Null(),
                                             c.alloc_hint,
                                             kvblock_size_,
                                             c.ns,
                                             c.acquire_flag);
                kvblock_size_ = kvblock_size;
                DCHECK_GE(std::numeric_limits<decltype(tagged_len_)>::max(),
                          tagged_len);
                tagged_len_ = tagged_len;
            }
            return RC::kOk;
        }
        void reset(IRdmaAdaptor *adpt, const MemHandleDecision &c)
        {
            if (raddr_.has_value())
            {
                DCHECK_GT(kvblock_size_, 0);
                adpt->remote_free(*raddr_, kvblock_size_, 0);
            }
            if (handle_.valid())
            {
                adpt->relinquish_perm(handle_, c.alloc_hint, c.relinquish_flag);
            }
            tagged_len_ = 0;
            kvblock_size_ = 0;
        }
    } to_insert_block;
    // struct
    // {
    //     // std::queue<GlobalAddress> raddrs_;
    //     GlobalAddress raddr_;
    //     RemoteMemHandle handle_;
    //     uint8_t cur_tagged_len_;
    //     size_t cur_kvblock_size_;

    // } to_insert_block;

    /**
     * this buffer is the DMA-able local buffer used to read/write.
     */
    // struct
    // {
    //     Buffer rdma_buffer_;
    //     size_t key_len_;
    //     size_t value_len_;
    // } usr_ctx_;

    RemoteMemHandle insert_kvblock_batch_handle_;
    GlobalAddress insert_kvblock_batch_raddr_;
    size_t alloc_idx_{0};

    // insert no batch
    size_t begin_insert_nr_{0};
    size_t end_insert_nr_{0};
    size_t free_insert_nr_{0};

    void do_free_kvblock([[maybe_unused]] GlobalAddress raddr, size_t size)
    {
        // TODO: this trigger one RPC call for freeing a KV block
        // auto rel_flag = (flag_t) LeaseModifyFlag::kServerDoNothing;
        // auto place_holder_handle = alloc_fake_handle();
        // rdma_adpt_->relinquish_perm(place_holder_handle, 0, rel_flag);
        // relinquish_fake_handle(place_holder_handle);
        rdma_adpt_->remote_free(raddr, size, 0 /* hint */);
    }

    size_t cached_gd() const
    {
        return cached_meta_.gd;
    }
    size_t cached_subtable_nr() const
    {
        return pow(2, cached_gd());
    }
    SubTableHandleT subtable_handle(size_t idx)
    {
        auto &st_mem_handle = get_subtable_mem_handle(idx);
        auto st_start_raddr = subtable_addr(idx);
        DCHECK_EQ(st_mem_handle.raddr(), st_start_raddr)
            << "call to get_subtable_mem_handle is expected to be consistent";
        SubTableHandleT st(st_start_raddr, st_mem_handle);
        return st;
    }
    // guaranteed to be valid and consistent
    RemoteMemHandle &get_subtable_mem_handle(size_t idx)
    {
        DCHECK_LT(idx, subtable_mem_handles_.size());
        auto &ret = subtable_mem_handles_[idx];
        if (unlikely(!ret.valid()))
        {
            build_subtable_mem_handle(idx);
            return get_subtable_mem_handle(idx);
        }
        auto mem_handle_start_raddr = ret.raddr();
        auto st_start_raddr = subtable_addr(idx);
        if (unlikely(mem_handle_start_raddr != st_start_raddr))
        {
            // inconsistency detected. rebuild the handle
            build_subtable_mem_handle(idx);
            return get_subtable_mem_handle(idx);
        }
        return ret;
    }
    void build_subtable_mem_handle(size_t idx)
    {
        const auto &c = conf_.meta.d;
        if (unlikely(subtable_mem_handles_[idx].valid()))
        {
            // exist old handle, free it before going on
            rdma_adpt_->relinquish_perm(
                subtable_mem_handles_[idx], c.alloc_hint, c.relinquish_flag);
        }

        DCHECK(!subtable_mem_handles_[idx].valid());
        subtable_mem_handles_[idx] =
            rdma_adpt_->acquire_perm(cached_meta_.entries[idx],
                                     c.alloc_hint,
                                     SubTableT::size_bytes(),
                                     c.ns,
                                     c.acquire_flag);
    }

    RemoteMemHandle &get_directory_mem_handle()
    {
        if (unlikely(!directory_mem_handle_.valid()))
        {
            init_directory_mem_handle();
        }
        return directory_mem_handle_;
    }
    uint32_t cached_ld(size_t idx)
    {
        return cached_meta_.lds[idx];
    }
    GlobalAddress subtable_addr(size_t idx)
    {
        return cached_meta_.entries[idx];
    }

    void init_directory_mem_handle()
    {
        const auto &c = conf_.meta.d;
        DCHECK(!directory_mem_handle_.valid());
        DCHECK(!table_meta_addr_.is_null());
        directory_mem_handle_ = rdma_adpt_->acquire_perm(table_meta_addr_,
                                                         c.alloc_hint,
                                                         sizeof(MetaT),
                                                         c.ns,
                                                         c.acquire_flag);
        DCHECK(directory_mem_handle_.valid());
    }
};

template <size_t E, size_t B, size_t S>
void RaceHashingHandleImpl<E, B, S>::init(util::TraceView trace)
{
    init_directory_mem_handle();
    trace.pin("init directory mem handle");

    update_directory_cache(trace).expect(RC::kOk);

    if (conf_.meta.eager_bind_subtable)
    {
        eager_init_subtable_mem_handle();
        trace.pin("eager init subtable mem handle");
    }

    inited_ = true;
    trace.pin("Finished");
}

template <size_t E, size_t B, size_t S>
void RaceHashingHandleImpl<E, B, S>::deinit(util::TraceView trace)
{
    const auto &c = conf_.meta.d;
    if (directory_mem_handle_.valid())
    {
        rdma_adpt_->relinquish_perm(
            directory_mem_handle_, c.alloc_hint, c.relinquish_flag);
    }
    trace.pin("rel directory_mem_handle");
    for (auto &handle : subtable_mem_handles_)
    {
        if (handle.valid())
        {
            rdma_adpt_->relinquish_perm(
                handle, c.alloc_hint, c.relinquish_flag);
            trace.pin("rel subtable_handle");
        }
    }
    if (fake_handle_.valid())
    {
        relinquish_fake_handle(fake_handle_);
        trace.pin("rel fake handle");
    }
    if (kvblock_region_handle_.valid())
    {
        const auto &c = conf_.kvblock_region.value();
        rdma_adpt_->relinquish_perm(
            kvblock_region_handle_, c.alloc_hint, c.relinquish_flag);
        trace.pin("rel kvblock handle");
    }

    end_read_kvblock();
    trace.pin("end_read_kvblock");
    if (to_insert_block.handle_.valid())
    {
        const auto &c = conf_.alloc_kvblock;
        rdma_adpt_->relinquish_perm(
            to_insert_block.handle_, c.alloc_hint, c.relinquish_flag);
        trace.pin("rel to_insert_block");
    }

    if (has_to_insert_block())
    {
        rdma_adpt_->remote_free(
            *to_insert_block.raddr_, to_insert_block.kvblock_size_, 0);
        consume_kv_block();
    }
    trace.pin("Finished");
}

template <size_t E, size_t B, size_t S>
void RaceHashingHandleImpl<E, B, S>::hack_trigger_rdma_protection_error()
{
    auto rdma_buffer = rdma_adpt_->get_rdma_buffer(64);
    auto &handle = get_directory_mem_handle();
    auto node_id = 0;
    auto ret = rdma_adpt_->rdma_write(
        GlobalAddress(node_id, -1), rdma_buffer.buffer, 64, handle);
    CHECK_EQ(ret, kOk);
    ret = rdma_adpt_->commit();
    LOG_IF(ERROR, ret != kRdmaProtectionErr)
        << "** Expect RdmaProtectionError. got: " << ret;

    rdma_adpt_->put_all_rdma_buffer();
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::update_directory_cache(
    util::TraceView trace)
{
    // TODO(race): this have performance problem because of additional
    // memcpy.
    auto rdma_buf = rdma_adpt_->get_rdma_buffer(meta_size());
    DCHECK_GE(rdma_buf.size, meta_size());
    RetCode rc;
    auto &dir_mem_handle = get_directory_mem_handle();
    rc = rdma_adpt_->rdma_read(rdma_buf.buffer,
                               table_meta_addr_,
                               meta_size(),
                               0 /* flag */,
                               dir_mem_handle);
    CHECK_EQ(rc, kOk);

    rdma_adpt_->commit(trace).expect(RC::kOk);

    memcpy(&cached_meta_, rdma_buf.buffer, meta_size());

    LOG_IF(INFO, config::kEnableDebug)
        << "[race][trace] update_directory_cache: update cache to "
        << cached_meta_ << ". " << util::pre(trace.kv());

    trace.pin("update_diretory_cache");
    return kOk;
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::del(const BufferView &key,
                                            util::TraceView trace)
{
    if (unlikely(!inited_))
    {
        init(trace);
    }
    DCHECK(inited_);
    auto hash = hash_impl(key.data(), key.size());
    auto m = hash_m(hash);
    auto fp = hash_fp(hash);
    auto rounded_m = round_to_bits(m, gd());
    auto [h1, h2] = hash_h1_h2(hash);

    DVLOG(V) << "[race] DEL key "
             << util::InlinedHexdump(key.data(), key.size()) << ", got hash "
             << pre_hash(hash) << ", m: " << m << ", rounded to " << rounded_m
             << " by cached_gd: " << gd() << ". fp: " << pre_fp(fp);
    auto subtable_idx = rounded_m;
    auto st = subtable_handle(subtable_idx);

    // get the two combined buckets the same time
    auto cbs = st.get_two_combined_bucket_handle(h1, h2, *rdma_adpt_);
    auto rc = rdma_adpt_->commit(trace);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk);

    std::unordered_set<SlotHandle> slot_handles;
    rc = cbs.locate(fp, cached_ld(subtable_idx), m, slot_handles, trace);
    if (rc == kCacheStale && auto_update_dir_)
    {
        // update cache and retry
        auto ret = update_directory_cache(trace);
        if (ret == kRdmaProtectionErr)
        {
            return ret;
        }
        CHECK_EQ(ret, kOk);
        return del(key, trace);
    }

    rc = remove_if_exists(subtable_idx, slot_handles, key, trace);
    if (rc == kOk)
    {
        DLOG_IF(INFO, config::kEnableDebug)
            << "[race][trace] del: rm at subtable[" << subtable_idx << "]. "
            << util::pre(trace.kv());
    }
    else
    {
        DLOG_IF(INFO, config::kEnableDebug)
            << "[race][trace] del: not found at subtable[" << subtable_idx
            << "]. " << util::pre(trace.kv());
    }
    return rc;
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::remove_if_exists(
    size_t subtable_idx,
    const std::unordered_set<SlotHandle> &slot_handles,
    const Key &key,
    util::TraceView trace)
{
    auto f = [this, subtable_idx](const Key &key,
                                  SlotHandle slot_handles,
                                  KVBlockHandle kvblock_handle,
                                  util::TraceView trace) {
        std::ignore = key;
        return do_remove(subtable_idx, slot_handles, kvblock_handle, trace);
    };
    return for_the_real_match_do(slot_handles, key, f, trace);
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::do_put_once(const Key &key,
                                                    uint64_t hash,
                                                    util::TraceView trace)
{
    DCHECK(inited_);

    auto [h1, h2] = hash_h1_h2(hash);
    auto fp = hash_fp(hash);
    auto m = hash_m(hash);
    auto cached_gd = gd();
    auto rounded_m = round_to_bits(m, cached_gd);
    auto ld = cached_ld(rounded_m);

    DCHECK(has_to_insert_block());
    auto rc = put_phase_one(key,
                            to_insert_block_raddr(),
                            to_insert_block_tagged_len(),
                            hash,
                            trace);
    if (rc != kOk)
    {
        if (rc == kCacheStale)
        {
            LOG(WARNING) << "Got at put_phase_one: " << PRE(rc);
        }
        return rc;
    }
    rc = phase_two_deduplicate(key, hash, ld, 10 /* retry nr */, trace);
    if (rc == kCacheStale)
    {
        LOG(WARNING) << "Got at phase_two_deduplicate: " << PRE(rc);
    }
    CHECK_NE(rc, kNoMem);
    CHECK_NE(rc, kRetry) << "** why dedup will require retry?";
    if constexpr (debug())
    {
        LOG_IF(INFO, config::kEnableDebug && rc == kOk)
            << "[race][trace] PUT succeed: hash: " << pre_hash(hash)
            << ", h1: " << pre_hash(h1) << ", h2: " << pre_hash(h2)
            << ", fp: " << pre_hash(fp) << ", m: " << pre_hash(m)
            << ", put to subtable[" << rounded_m << "] by gd " << cached_gd
            << ". cached_ld: " << ld << ". " << util::pre(trace.kv());
    }
    return rc;
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::do_put_loop(const Key &key,
                                                    uint64_t hash,
                                                    util::TraceView trace)
{
    RetCode rc = kOk;
    size_t retry_nr{0};

    while (true)
    {
        retry_nr++;
        CHECK_LT(retry_nr, 102400) << "** Failed to many times";

        if (rc == RC::kCacheStale)
        {
            // not my deal to handle this
            if (!auto_update_dir_)
            {
                trace.pin("kCacheStale");
                return rc;
            }

            rc = update_directory_cache(trace);
            CHECK_EQ(rc, kOk);
            // retry
            continue;
        }
        if (rc == kNoMem)
        {
            // not my deal to handle this
            if (!auto_expand_)
            {
                trace.pin("kNoMem");
                return rc;
            }

            auto m = hash_m(hash);
            auto cached_gd = gd();
            auto rounded_m = round_to_bits(m, cached_gd);
            auto ld = cached_ld(rounded_m);
            auto overflow_subtable_idx = round_to_bits(rounded_m, ld);
            rc = expand(overflow_subtable_idx, trace);
            // got kNoMem AGAIN from expand, which means unable to
            // expand dont retry more. exit here.
            if (rc == kNoMem)
            {
                trace.pin("kNoMem");
                return rc;
            }
            // special case, just ignore me.
            if (unlikely(rc == RC::kMockCrashed))
            {
                trace.pin("kMockCrashed");
                return rc;
            }
            // otherwise, keep the retry-ing.
            CHECK(rc == kRetry || rc == kOk || rc == kCacheStale)
                << "** rc can be only retry or ok or cache-stale. got: " << rc;
            continue;
        }

        // insert here
        CHECK(rc == kOk || rc == kRetry) << "** Unexpected rc: " << rc;
        rc = do_put_once(key, hash, trace);
        if (rc == kOk)
        {
            trace.pin("OK");
            return rc;
        }

        CHECK(rc == kNoMem || rc == kRetry || rc == kCacheStale)
            << "** Only allow kNoMem, kRetry, kCacheStale. got: " << rc;
    }
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::put(KVBlockView &kvblock_view,
                                            util::TraceView trace)
{
    if (unlikely(!inited_))
    {
        init(trace);
    }
    DCHECK(inited_);

    auto key = kvblock_view.key_view();
    auto value = kvblock_view.value_view();

    // need allocation
    auto rc = prepare_alloc_kv(key, value);
    if (unlikely(rc != RC::kOk))
    {
        DCHECK_EQ(rc, RC::kNoMem);
        return rc;
    }

    DCHECK(has_to_insert_block());
    // NOTE: the prepare_write_kv is doing once for each put request
    auto hash = hash_impl(kvblock_view.key_buf(), kvblock_view.key_len());
    *kvblock_view.hash_buf() = hash;

    prepare_write_kv(kvblock_view,
                     to_insert_block_raddr(),
                     to_insert_block_kvblock_size(),
                     to_insert_block_handle(),
                     *rdma_adpt_,
                     trace)
        .expect(RC::kOk);

    // Here we will loop until insertion succeeded (best-efford)
    trace.pin("allocate");
    rc = do_put_loop(key, hash, trace);

    if (rc == RC::kOk)
    {
        consume_kv_block();
        trace.pin("consume kv block");
        return rc;
    }
    // retry soo many times but still failed by do_put_loop
    // will not retry anymore.
    if (rc == RC::kRetry || rc == RC::kNoMem || rc == RC::kMockCrashed)
    {
        return rc;
    }

    avis::avis_debug();

    LOG(FATAL) << "** Unexpected and unhandled rc here. rc: " << rc;
    return RC::kInvalid;
}

template <size_t E, size_t B, size_t S>
bool RaceHashingHandleImpl<E, B, S>::maybe_expand_try_extend_lock_lease(
    size_t subtable_idx, [[maybe_unused]] util::TraceView trace)
{
    const auto &c = conf_.expand;
    if (unlikely(!c.use_patronus_lock))
    {
        return true;
    }
    auto now = std::chrono::steady_clock::now();
    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      now - lock_handle_last_extend_)
                      .count();
    bool should_extend = ns >= util::time::to_ns(c.lock_time_ns) / 2;
    if (unlikely(should_extend))
    {
        auto rc = rdma_adpt_->extend(expand_lock_handles_[subtable_idx],
                                     c.lock_time_ns);
        CHECK_EQ(rc, kOk) << "Failed to extend.";
        lock_handle_last_extend_ = now;
        if (rc == kOk)
        {
            DLOG_IF(INFO, config::kEnableExpandDebug)
                << "[race][expand] maybe_expand_try_extend_lock_lease: "
                   "extend SUCCEEDED. ";
            return true;
        }
        else
        {
            DLOG_IF(INFO, config::kEnableExpandDebug)
                << "[race][expand] maybe_expand_try_extend_lock_lease: "
                   "extend FAILED ";
            return false;
        }
    }
    else
    {
        DLOG_IF(INFO, config::kEnableExpandDebug)
            << "[race][expand] maybe_expand_try_extend_lock_lease: "
               "extend SKIP. elapsed "
            << ns << " ns, not close to expire time.";
    }
    return true;
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::expand_try_lock_subtable_drain(
    size_t subtable_idx)
{
    const auto &c = conf_.expand;
    if (unlikely(c.use_patronus_lock))
    {
        // a little bit hack
        // get another lease to utilize the lock & fault tolerant semantics
        expand_lock_handles_[subtable_idx] =
            rdma_adpt_->acquire_perm(lock_remote_addr(subtable_idx),
                                     0,
                                     8,
                                     c.lock_time_ns,
                                     c.patronus_lock_flag);
        if (!expand_lock_handles_[subtable_idx].valid())
        {
            CHECK_EQ(expand_lock_handles_[subtable_idx].ec(),
                     AcquireRequestStatus::kLockedErr);
            return kRetry;
        }

        lock_handle_last_extend_ = std::chrono::steady_clock::now();
        return kOk;
    }
    else
    {
        auto rdma_buf = rdma_adpt_->get_rdma_buffer(8);
        DCHECK_GE(rdma_buf.size, 8);
        auto remote = lock_remote_addr(subtable_idx);
        uint64_t expect = 0;   // no lock
        uint64_t desired = 1;  // lock
        auto &dir_mem_handle = get_directory_mem_handle();
        rdma_adpt_
            ->rdma_cas(remote,
                       expect,
                       desired,
                       rdma_buf.buffer,
                       0 /* flag */,
                       dir_mem_handle)
            .expect(RC::kOk);
        auto rc = rdma_adpt_->commit();
        if (unlikely(rc == kRdmaProtectionErr))
        {
            return rc;
        }
        CHECK_EQ(rc, kOk);
        uint64_t r = *(uint64_t *) rdma_buf.buffer;
        CHECK(r == 0 || r == 1)
            << "Unexpected value read from lock of subtable[" << subtable_idx
            << "]. Expect 0 or 1, got " << r;
        if (r == 0)
        {
            DCHECK_EQ(desired, 1);
            cached_meta_.expanding[subtable_idx] = desired;
            return kOk;
        }
        return kRetry;
    }
}

template <size_t kDEntryNr, size_t kBucketGroupNr, size_t kSlotNr>
inline std::ostream &operator<<(
    std::ostream &os,
    const RaceHashingHandleImpl<kDEntryNr, kBucketGroupNr, kSlotNr> &rhh)
{
    os << "RaceHashingHandleImpl gd: " << rhh.cached_gd() << std::endl;
    for (size_t i = 0; i < rhh.cached_subtable_nr(); ++i)
    {
        os << "sub-table[" << i << "]: ld: " << rhh.cached_meta_.lds[i]
           << ", at " << rhh.cached_meta_.entries[i] << std::endl;
    }
    return os;
}

template <size_t E, size_t B, size_t kSlotNr>
RetCode RaceHashingHandleImpl<E, B, kSlotNr>::expand_migrate_subtable(
    SubTableHandleT &src_st_handle,
    SubTableHandleT &dst_st_handle,
    size_t bit,
    util::TraceView trace)
{
    std::unordered_set<SlotMigrateHandle> should_migrate;
    auto st_size = SubTableT::size_bytes();
    auto rdma_buf = rdma_adpt_->get_rdma_buffer(st_size);
    DCHECK_GE(rdma_buf.size, st_size);
    rdma_adpt_
        ->rdma_read(rdma_buf.buffer,
                    src_st_handle.raddr(),
                    st_size,
                    0 /* flag */,
                    src_st_handle.mem_handle())
        .expect(RC::kOk);
    auto rc = rdma_adpt_->commit(trace);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk);

    if (unlikely(!fake_handle_.valid()))
    {
        fake_handle_ = alloc_fake_handle();
    }

    for (size_t i = 0; i < SubTableHandleT::kTotalBucketNr; ++i)
    {
        auto remote_bucket_addr =
            src_st_handle.raddr() + i * Bucket<kSlotNr>::size_bytes();
        void *bucket_buffer_addr =
            (char *) rdma_buf.buffer + i * Bucket<kSlotNr>::size_bytes();
        BucketHandle<kSlotNr> b(remote_bucket_addr,
                                (char *) bucket_buffer_addr);
        CHECK_EQ(b.should_migrate(
                     bit, *rdma_adpt_, fake_handle_, should_migrate, trace),
                 kOk);
    }

    // TODO: rethink about how batching can boost performance
    auto capacity = SubTableT::max_capacity();
    DLOG_IF(INFO, config::kEnableExpandDebug)
        << "[race][expand] should migrate " << should_migrate.size()
        << " entries. subtable capacity: " << capacity << ". "
        << util::pre(trace.kv());
    CHECK_LE(should_migrate.size(), capacity)
        << "Should migrate got " << should_migrate.size()
        << ", exceeded subtable capacity " << capacity;

    if constexpr (debug())
    {
        LOG_FIRST_N(WARNING, 1) << "TODO: expand_migrate_subtable: "
                                   "put_slot will read cb each time "
                                   "for one entry migration.";
    }

    for (auto slot_handle : should_migrate)
    {
        SlotHandle ret_slot(GlobalAddress::Null(), SlotView(0));
        CHECK_EQ(
            dst_st_handle.put_slot(slot_handle, *rdma_adpt_, &ret_slot, trace),
            kOk);
        auto rc =
            src_st_handle.try_del_slot(slot_handle.slot_handle(), *rdma_adpt_);
        if (rc != kOk)
        {
            DCHECK(!ret_slot.ptr().is_null());
            dst_st_handle.del_slot(ret_slot, *rdma_adpt_);
        }
        rdma_adpt_->put_all_rdma_buffer();
    }

    return kOk;
}

template <size_t kDEntryNr, size_t B, size_t S>
RetCode RaceHashingHandleImpl<kDEntryNr, B, S>::expand(size_t subtable_idx,
                                                       util::TraceView trace)
{
    if constexpr (::config::kEnableRdmaTrace)
    {
        if (likely(!rdma_adpt_->trace_enabled()))
        {
            if (true_with_prob(::config::kRdmaTraceRateExpand))
            {
                rdma_adpt_->enable_trace("put-expand");
            }
        }
    }

    DCHECK(inited_);
    CHECK_LT(subtable_idx, kDEntryNr);
    // 0) try lock
    auto rc = expand_try_lock_subtable_drain(subtable_idx);
    trace.pin("expand_try_lock_subtable_drain");
    if (rc != kOk)
    {
        // failed migration, don't trace me.
        return rc;
    }

    // NOTE:
    // Here, right after successfully locking the subtable
    // we do the fault tolerance test of Patronus
    // let the client crashes here.
    if (unlikely(conf_.expand.mock_crash_nr > 0))
    {
        LOG(WARNING) << "[rhh] MOCK: client crashed. ";
        conf_.expand.mock_crash_nr--;

        return kMockCrashed;
    }

    // 1) I think updating the directory cache is definitely necessary
    // while expanding.
    auto ret = update_directory_cache(trace);
    if (ret == kRdmaProtectionErr)
    {
        return ret;
    }
    CHECK_EQ(ret, kOk);
    auto depth = cached_meta_.lds[subtable_idx];
    auto next_subtable_idx = subtable_idx | (1 << depth);
    DLOG_IF(INFO, config::kEnableExpandDebug)
        << "[race][expand] Expanding subtable[" << subtable_idx
        << "] to subtable[" << next_subtable_idx << "]. meta: " << cached_meta_
        << ". trace: " << util::pre(trace.kv());
    auto subtable_remote_addr = cached_meta_.entries[subtable_idx];
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);

    // okay, entered critical section
    auto next_depth = depth + 1;
    auto expect_gd = gd();
    if (pow(2, next_depth) > kDEntryNr)
    {
        // DLOG(WARNING) << "[race][expand] (1) trying to expand to gd "
        //               << next_depth << " with entries "
        //               << pow(2, next_depth) << ". Out of directory
        //               entry.";
        auto rc = expand_unlock_subtable_nodrain(subtable_idx);
        if (unlikely(rc == kRdmaProtectionErr))
        {
            return rc;
        }
        CHECK_EQ(rc, kOk);
        rc = rdma_adpt_->commit(trace);
        if (unlikely(rc == kRdmaProtectionErr))
        {
            return rc;
        }
        CHECK_EQ(rc, kOk);
        if constexpr (debug())
        {
            if (!conf_.expand.use_patronus_lock)
            {
                CHECK_EQ(cached_meta_.expanding[subtable_idx], 1);
            }
        }
        cached_meta_.expanding[subtable_idx] = 0;
        return kNoMem;
    }
    rdma_adpt_->put_all_rdma_buffer();
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);

    // 2) Expand the directory first
    if (next_depth >= expect_gd)
    {
        // insert all the entries into the directory
        // before updating gd
        for (size_t i = 0; i < pow(2, next_depth); ++i)
        {
            if (cached_meta_.entries[i].is_null())
            {
                auto from_subtable_idx = i & (~(1 << depth));
                DLOG_IF(INFO, config::kEnableExpandDebug)
                    << "[race][expand] (2) Update gd and directory: "
                       "setting subtable["
                    << i << "] to subtale[" << from_subtable_idx << "]. "
                    << util::pre(trace.kv());
                auto subtable_remote_addr =
                    cached_meta_.entries[from_subtable_idx];
                auto ld = cached_ld(from_subtable_idx);
                auto rc = expand_write_entry_nodrain(i, subtable_remote_addr);
                if (unlikely(rc == kRdmaProtectionErr))
                {
                    return rc;
                }
                CHECK_EQ(rc, kOk);
                rc = expand_update_ld_nodrain(i, ld);
                if (unlikely(rc == kRdmaProtectionErr))
                {
                    return rc;
                }
                CHECK_EQ(rc, kOk);
                // TODO(race): update cache here. Not sure if it is right
                cached_meta_.entries[i] = subtable_remote_addr;
                cached_meta_.lds[i] = ld;
            }
        }
        auto rc = rdma_adpt_->commit(trace);
        if (unlikely(rc == kRdmaProtectionErr))
        {
            return rc;
        }
        CHECK_EQ(rc, kOk);

        DLOG_IF(INFO, config::kEnableExpandDebug)
            << "[race][expand] (2) Update gd and directory: setting gd to "
            << next_depth;
        rc = expand_cas_gd_drain(expect_gd, next_depth);
        if (unlikely(rc == kRdmaProtectionErr))
        {
            return rc;
        }
        CHECK_EQ(rc, kOk);
    }
    trace.pin("expand directory");

    CHECK_LT(next_subtable_idx, kDEntryNr);
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);

    // do some debug checks
    auto cached_meta_next_subtable = cached_meta_.entries[next_subtable_idx];
    DCHECK_EQ(cached_meta_next_subtable.nodeID, 0);
    auto cached_meta_subtable = cached_meta_.entries[subtable_idx];
    DCHECK_EQ(cached_meta_subtable.nodeID, 0);
    if (!cached_meta_next_subtable.is_null() &&
        cached_meta_next_subtable != cached_meta_subtable)
    {
        CHECK(false) << "Failed to extend: already have subtables here. "
                        "Trying to expand subtable["
                     << subtable_idx << "] to subtable[" << next_subtable_idx
                     << "]. But already exists subtable at "
                     << cached_meta_next_subtable
                     << ", which is not nullptr or " << cached_meta_subtable
                     << " from subtable[" << subtable_idx << "]"
                     << ". depth: " << depth
                     << ", (1<<depth): " << (1 << depth);
    }
    rdma_adpt_->put_all_rdma_buffer();
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);

    // 3) allocate subtable here
    auto alloc_size = SubTableT::size_bytes();
    if (subtable_mem_handles_[next_subtable_idx].valid())
    {
        const auto &c = conf_.meta.d;
        rdma_adpt_->relinquish_perm(subtable_mem_handles_[next_subtable_idx],
                                    c.alloc_hint,
                                    c.relinquish_flag);
    }
    subtable_mem_handles_[next_subtable_idx] =
        remote_alloc_acquire_subtable_directory(alloc_size);
    auto &next_subtable_handle = subtable_mem_handles_[next_subtable_idx];
    auto new_remote_subtable = subtable_mem_handles_[next_subtable_idx].raddr();
    LOG_IF(INFO, config::kEnableExpandDebug)
        << "[race][expand] (3) expanding subtable[" << subtable_idx
        << "] to next subtable[" << next_subtable_idx << "]. Allocated "
        << alloc_size << " at " << new_remote_subtable;
    rdma_adpt_->put_all_rdma_buffer();
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);
    trace.pin("allocate subtable");

    // 4) init subtable: setup the bucket header
    auto ld = next_depth;
    auto suffix = next_subtable_idx;
    LOG_IF(INFO, config::kEnableExpandDebug)
        << "[race][expand] (4) set up header for subtable[" << next_subtable_idx
        << "]. ld: " << ld << ", suffix: " << next_subtable_idx;
    // should remotely memset the buffer to zero
    // then set up the header.
    SubTableHandleT new_remote_st_handle(new_remote_subtable,
                                         next_subtable_handle);
    rc = expand_init_and_update_remote_bucket_header_drain(
        new_remote_st_handle, ld, suffix, trace);
    CHECK_EQ(rc, kOk);
    cached_meta_.lds[next_subtable_idx] = ld;

    rdma_adpt_->put_all_rdma_buffer();
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);
    trace.pin("4) init subtable");

    // 5) insert the subtable into the directory AND lock the subtable.
    DLOG_IF(INFO, config::kEnableExpandDebug)
        << "[race][expand] (5) Lock subtable[" << next_subtable_idx
        << "] at directory. Insert subtable " << new_remote_subtable
        << " into directory. Update ld to " << ld << " for subtable["
        << subtable_idx << "] and subtable[" << next_subtable_idx << "]";
    rc = expand_try_lock_subtable_drain(next_subtable_idx);
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk) << "Should not be conflict with concurrent expand.";
    CHECK_EQ(
        expand_install_subtable_nodrain(next_subtable_idx, new_remote_subtable),
        kOk);
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);

    rc = expand_update_ld_nodrain(subtable_idx, ld);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk);
    rc = expand_update_ld_nodrain(next_subtable_idx, ld);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk);
    rc = rdma_adpt_->commit(trace);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk);
    // update local cache
    cached_meta_.entries[next_subtable_idx] = new_remote_subtable;
    cached_meta_.lds[subtable_idx] = ld;
    cached_meta_.lds[next_subtable_idx] = ld;

    rdma_adpt_->put_all_rdma_buffer();
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);
    trace.pin("5) allocate subtable");

    // 6) move data.
    // 6.1) update bucket suffix
    DLOG_IF(INFO, config::kEnableExpandDebug)
        << "[race][expand] (6.1) update bucket header for subtable["
        << subtable_idx << "]. ld: " << ld << ", suffix: " << subtable_idx;
    // auto *origin_subtable = entries_[subtable_idx];
    // origin_subtable->update_header(ld, subtable_idx);
    auto origin_subtable_remote_addr = cached_meta_.entries[subtable_idx];
    auto &org_st_mem_handle = get_subtable_mem_handle(subtable_idx);
    SubTableHandleT origin_subtable_handle(origin_subtable_remote_addr,
                                           org_st_mem_handle);
    CHECK_EQ(expand_update_remote_bucket_header_drain(
                 origin_subtable_handle, ld, subtable_idx, trace),
             kOk);
    cached_meta_.lds[subtable_idx] = ld;

    rdma_adpt_->put_all_rdma_buffer();
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);
    trace.pin("6.1) update bucket suffix");

    // Before 6.1) Iterate all the entries in @entries_, check for any
    // recursive updates to the entries.
    // For example, when
    // subtable_idx in {1, 5, 9, 13} pointing to the same LD = 2, suffix =
    // 0b01, when expanding subtable 1 to 5, should also set 9 pointing to 1
    // (not changed) and 13 pointing to 5 (changed)
    // 6.2)
    DLOG_IF(INFO, config::kEnableExpandDebug)
        << "[race][expand] (6.1) recursively checks all the entries "
           "for further updates. "
        << util::pre(trace.kv());
    auto &dir_mem_handle = get_directory_mem_handle();
    CHECK_EQ(expand_cascade_update_entries_drain(
                 new_remote_subtable,
                 subtable_remote_addr,
                 ld,
                 round_to_bits(next_subtable_idx, ld),
                 *rdma_adpt_,
                 dir_mem_handle,
                 trace),
             kOk);
    rdma_adpt_->put_all_rdma_buffer();
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);

    trace.pin("6.2) cascade update entries");

    // 6.3) insert all items from the old bucket to the new
    DLOG_IF(INFO, config::kEnableExpandDebug)
        << "[race][expand] (6.3) migrate slots from subtable[" << subtable_idx
        << "] to subtable[" << next_subtable_idx << "] with the " << ld
        << "-th bit == 1. " << util::pre(trace.kv());
    auto bits = ld;
    CHECK_GE(bits, 1) << "Test the " << bits
                      << "-th bits, which index from one.";
    auto &src_st_mem_handle = org_st_mem_handle;
    auto &dst_st_mem_handle = next_subtable_handle;
    SubTableHandleT org_st(origin_subtable_remote_addr, src_st_mem_handle);
    SubTableHandleT dst_st(new_remote_subtable, dst_st_mem_handle);
    rc = expand_migrate_subtable(org_st, dst_st, bits - 1, trace);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk);
    rdma_adpt_->put_all_rdma_buffer();
    maybe_expand_try_extend_lock_lease(subtable_idx, trace);

    trace.pin("6.3) migrate slots");

    // 7) unlock
    DLOG_IF(INFO, config::kEnableExpandDebug)
        << "[race][expand] (7) Unlock subtable[" << next_subtable_idx
        << "] and subtable[" << subtable_idx << "]. " << util::pre(trace.kv());
    rc = expand_unlock_subtable_nodrain(next_subtable_idx);
    CHECK_EQ(rc, kOk);
    rc = expand_unlock_subtable_nodrain(subtable_idx);
    CHECK_EQ(rc, kOk);
    rc = rdma_adpt_->commit(trace);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk);
    cached_meta_.expanding[next_subtable_idx] = 0;
    cached_meta_.expanding[subtable_idx] = 0;
    trace.pin("7) unlock");

    if constexpr (debug())
    {
        print_latest_meta_image(trace);
    }
    return kOk;
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::phase_two_deduplicate(
    const Key &key,
    uint64_t hash,
    uint32_t cached_ld,
    ssize_t retry_nr,
    util::TraceView trace)
{
    if (unlikely(retry_nr <= 0))
    {
        LOG(FATAL) << "** This is wierd. Tried many times for "
                      "deduplication, but always found cache stale.";
        // return kCacheStale;
        return kOk;
    }
    auto m = hash_m(hash);
    auto rounded_m = round_to_bits(m, gd());
    auto [h1, h2] = hash_h1_h2(hash);
    auto fp = hash_fp(hash);

    auto subtable_idx = rounded_m;

    auto st = subtable_handle(subtable_idx);

    // get the two combined buckets the same time
    // validate staleness at the same time
    auto cbs = st.get_two_combined_bucket_handle(h1, h2, *rdma_adpt_);
    auto rc = rdma_adpt_->commit(trace);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk);

    std::unordered_set<SlotHandle> slot_handles;
    rc = cbs.locate(fp, cached_ld, m, slot_handles, trace);
    DLOG_IF(WARNING, slot_handles.size() >= 5)
        << "** Got a lots of real match (" << slot_handles.size()
        << "). m: " << m << ", rounded_m: " << rounded_m
        << ", h1: " << pre_hash(h1) << ", h2: " << pre_hash(h2)
        << ", fp: " << pre_hash(fp) << ". key: " << key
        << ". Got possible match nr: " << slot_handles.size();
    if (rc == kCacheStale && auto_update_dir_)
    {
        // update cache and retry
        update_directory_cache(trace).expect(RC::kOk);
        return phase_two_deduplicate(key, hash, cached_ld, retry_nr - 1, trace);
    }

    // duplicate existed. Do the deduplication
    if (slot_handles.size() >= 2)
    {
        std::unordered_set<Location> real_match_handles;
        auto rc = get_real_match_handles(
            slot_handles, key, real_match_handles, trace);
        CHECK(rc == kOk || rc == kNotFound) << "Unexpected rc: " << rc;
        if (real_match_handles.size() <= 1)
        {
            return kOk;
        }
        auto chosen_slot_it = deterministic_choose_slot(real_match_handles);
        for (const auto &view : real_match_handles)
        {
            if (view != *chosen_slot_it)
            {
                auto rc = do_remove(
                    subtable_idx, view.slot_handle, view.kvblock_handle, trace);
                CHECK(rc == kOk || rc == kRetry);
            }
        }
    }
    trace.pin("Finished");
    return kOk;
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::do_remove(size_t subtable_idx,
                                                  SlotHandle slot_handle,
                                                  KVBlockHandle kvblock_handle,
                                                  util::TraceView trace)
{
    auto expect_slot = slot_handle.slot_view();
    uint64_t expect_val = expect_slot.val();
    auto desired_slot = slot_handle.view_after_clear();
    auto rdma_buf = rdma_adpt_->get_rdma_buffer(8);
    DCHECK_GE(rdma_buf.size, 8);
    auto &subtable_mem_handle = get_subtable_mem_handle(subtable_idx);
    if (avis::Config::ins().enable_ptl())
    {
        auto ptl = rdma_adpt_->ptl();
        if (ptl)
        {
            ptl->record_free(slot_handle.ptr());
        }
    }

    rdma_adpt_
        ->rdma_cas(slot_handle.remote_addr(),
                   expect_val,
                   desired_slot.val(),
                   rdma_buf.buffer,
                   0 /* flag */,
                   subtable_mem_handle)
        .expect(RC::kOk);
    auto rc = rdma_adpt_->commit(trace);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk);
    bool success = memcmp(rdma_buf.buffer, &expect_val, 8) == 0;
    if (success)
    {
        DLOG_IF(INFO, config::kEnableDebug)
            << "[race][trace] do_remove SUCC: clearing slot " << slot_handle
            << ". kOk: " << util::pre(trace.kv());
        do_free_kvblock(expect_slot.ptr(), kvblock_handle.total_size());
        return kOk;
    }
    else
    {
        DLOG_IF(INFO, config::kEnableDebug)
            << "[race][trace] do_remove FAILED: clearing slot " << slot_handle
            << ". kRetry. " << util::pre(trace.kv());
        return kRetry;
    }
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::put_phase_one(const Key &key,
                                                      GlobalAddress kv_block,
                                                      size_t len,
                                                      uint64_t hash,
                                                      util::TraceView trace)
{
    RetCode rc = kOk;
    auto m = hash_m(hash);
    auto fp = hash_fp(hash);
    auto [h1, h2] = hash_h1_h2(hash);
    auto rounded_m = round_to_bits(m, gd());
    auto ld = cached_ld(rounded_m);
    DLOG_IF(INFO, config::kEnableDebug)
        << "[race][trace] put_phase_one: got hash " << pre_hash(hash)
        << ", m: " << m << ", rounded to " << rounded_m
        << " (subtable) by gd(may stale): " << gd() << ". fp: " << pre_fp(fp)
        << ", cached_ld: " << ld;
    auto st = subtable_handle(rounded_m);
    trace.pin("subtable handle");

    DCHECK_EQ(kv_block.nodeID, 0)
        << "The raddr we got here should have been transformed: the upper "
           "bits should be zeros";
    SlotView new_slot(fp, len, kv_block);

    auto cbs = st.get_two_combined_bucket_handle(h1, h2, *rdma_adpt_);
    rdma_adpt_->commit(trace).expect(RC::kOk);
    trace.pin("fetched combined bucket");

    DLOG_IF(INFO, config::kEnableMemoryDebug)
        << "[race][mem] allocated kv_block raddr: " << kv_block
        << " with len: " << len
        << ". actual len: " << new_slot.actual_len_bytes();

    std::unordered_set<SlotHandle> slot_handles;
    rc = cbs.locate(fp, ld, m, slot_handles, trace);
    if (rc == kCacheStale)
    {
        LOG(WARNING) << "get at cbs.locate: " << PRE(rc);
    }
    CHECK_NE(rc, kNotFound) << "Even not found should not response not found";
    if (rc != kOk)
    {
        CHECK_EQ(rc, kCacheStale) << "** cbs.locate only allow kCacheStale err";
        return rc;
    }

    rc = update_if_exists(rounded_m, slot_handles, key, new_slot, trace);
    if (rc == kCacheStale)
    {
        LOG(INFO) << "get at update_if_exists: " << PRE(rc);
    }

    if (rc == kOk)
    {
        return rc;
    }
    // also another way of "kOk", we succeeded but found nothing
    else if (rc == kNotFound)
    {
        rc = insert_if_exist_empty_slot(
            rounded_m, cbs, cached_ld(rounded_m), m, new_slot, trace);
        CHECK_NE(rc, kRetry)
            << "** insert_if_exist_empty_slot should do the retry itself.";
        CHECK(rc == kOk || rc == kNoMem);
        return rc;
    }
    else
    {
        CHECK_EQ(rc, kRetry);
        return rc;
    }
}

template <size_t E, size_t B, size_t kSlotNr>
RetCode RaceHashingHandleImpl<E, B, kSlotNr>::insert_if_exist_empty_slot(
    size_t subtable_idx,
    const TwoCombinedBucketHandle<kSlotNr> &cb,
    uint32_t ld,
    uint32_t suffix,
    SlotView new_slot,
    util::TraceView trace)
{
    std::vector<BucketHandle<kSlotNr>> buckets;
    buckets.reserve(4);
    auto rc = cb.get_bucket_handle(buckets);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk);
    for (auto &bucket : buckets)
    {
        RetCode rc;
        if ((rc = bucket.validate_staleness(ld, suffix, trace)) != kOk)
        {
            DCHECK(rc == kCacheStale || rc == kRdmaProtectionErr)
                << "** unexpected rc: " << rc;
            trace.pin("CacheStale");
            return rc;
        }
        auto poll_slot_idx = fast_pseudo_rand_int(1, kSlotNr - 1);
        constexpr auto kDataSlotNr = Bucket<kSlotNr>::kDataSlotNr;
        for (size_t i = 0; i < kDataSlotNr; ++i)
        {
            auto idx = (poll_slot_idx + i) % kDataSlotNr + 1;
            DCHECK_GE(idx, 1);
            DCHECK_LT(idx, kSlotNr);
            auto view = bucket.slot_view(idx);
            if (view.empty())
            {
                auto &st_mem_handle = get_subtable_mem_handle(subtable_idx);
                auto rc = bucket.do_insert(bucket.slot_handle(idx),
                                           new_slot,
                                           *rdma_adpt_,
                                           st_mem_handle,
                                           nullptr,
                                           trace);
                if (rc == kOk)
                {
                    trace.pin("OK");
                    return kOk;
                }
                DCHECK_EQ(rc, kRetry);
            }
        }
    }
    return kNoMem;
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::expand_cascade_update_entries_drain(
    GlobalAddress next_subtable_addr,
    GlobalAddress origin_subtable_addr,
    uint32_t ld,
    uint32_t suffix,
    IRdmaAdaptor &rdma_adpt,
    RemoteMemHandle &dir_mem_handle,
    util::TraceView trace)
{
    DCHECK_EQ(next_subtable_addr.nodeID, 0);
    DCHECK_EQ(origin_subtable_addr.nodeID, 0);
    for (size_t i = 0; i < pow((size_t) 2, (size_t) cached_meta_.gd); ++i)
    {
        auto subtable_addr = cached_meta_.entries[i];
        auto rounded_i = round_to_bits(i, ld);
        // DLOG_IF(INFO, config::kEnableExpandDebug )
        //     << "[race][trace] expand_cascade_update_entries_drain: "
        //        "subtable["
        //     << i << "] at " << (void *) subtable_addr
        //     << ", test match with " << (void *) origin_subtable_addr
        //     << ", rounded_i: " << rounded_i << ", test match with "
        //     << suffix;
        if (subtable_addr == origin_subtable_addr)
        {
            // if addr match, should update ld.
            auto ld_remote = ld_remote_addr(i);
            auto ld_rdma_buf = rdma_adpt.get_rdma_buffer(sizeof(uint32_t));
            DCHECK_GE(ld_rdma_buf.size, sizeof(uint32_t));
            *(uint32_t *) ld_rdma_buf.buffer = ld;
            rdma_adpt
                .rdma_write(ld_remote,
                            ld_rdma_buf.buffer,
                            sizeof(uint32_t),
                            0 /* flag */,
                            dir_mem_handle)
                .expect(RC::kOk);
            cached_meta_.lds[i] = ld;
            DLOG_IF(INFO, config::kEnableExpandDebug)
                << "[race][trace] expand_cascade_update_entries_drain: "
                   "UPDATE LD "
                   "subtable["
                << i << "] update LD to " << ld << ". "
                << util::pre(trace.kv());

            // if suffix match, further update entries
            if (rounded_i == suffix)
            {
                // for entry
                auto entry_rdma_buf =
                    rdma_adpt.get_rdma_buffer(sizeof(next_subtable_addr));
                DCHECK_GE(entry_rdma_buf.size, sizeof(next_subtable_addr));
                *(uint64_t *) entry_rdma_buf.buffer = next_subtable_addr.val;
                auto entry_remote = entries_remote_addr(i);
                rdma_adpt
                    .rdma_write(entry_remote,
                                entry_rdma_buf.buffer,
                                sizeof(next_subtable_addr),
                                0 /* flag */,
                                dir_mem_handle)
                    .expect(RC::kOk);
                DLOG_IF(INFO, config::kEnableExpandDebug)
                    << "[race][trace] expand_cascade_update_entries_drain: "
                       "UPDATE ENTRY "
                       "subtable["
                    << i << "] update entry from " << origin_subtable_addr
                    << " to " << next_subtable_addr << ". "
                    << util::pre(trace.kv());
            }
        }
    }
    auto rc = rdma_adpt_->commit(trace);
    if (unlikely(rc == kRdmaProtectionErr))
    {
        return rc;
    }
    CHECK_EQ(rc, kOk);
    return kOk;
}

template <size_t E, size_t B, size_t S>
template <typename Fn>
RetCode RaceHashingHandleImpl<E, B, S>::for_the_real_match_do(
    const std::unordered_set<SlotHandle> &slot_handles,
    const Key &key,
    Fn &&func,
    util::TraceView trace)
{
    std::map<SlotHandle, KVBlockHandle> slots_rdma_buffers;
    // TODO: maybe enable batching here. Patronus API?
    // debug_fp_conflict_m_.collect(slot_handles.size());

    // bool has_real_match = false;
    for (const auto &slot_handle : slot_handles)
    {
        size_t actual_size = slot_handle.slot_view().actual_len_bytes();
        DCHECK_GT(actual_size, 0)
            << "make no sense to have actual size == 0. slot_handle: "
            << slot_handle;
        auto rdma_buffer = rdma_adpt_->get_rdma_buffer(actual_size);
        DCHECK_GE(rdma_buffer.size, actual_size);

        auto remote_kvblock_addr = slot_handle.ptr();
        DLOG_IF(INFO, config::kEnableMemoryDebug)
            << "[race][mem] Reading remote kvblock addr: "
            << remote_kvblock_addr << " with size " << actual_size
            << ", fp: " << pre_fp(slot_handle.fp());

        auto &kvblock_handle =
            begin_read_kvblock(remote_kvblock_addr, actual_size);
        trace.pin("begin read kvblock");

        rdma_adpt_
            ->rdma_read((char *) rdma_buffer.buffer,
                        remote_kvblock_addr,
                        actual_size,
                        0 /* flag */,
                        kvblock_handle)
            .expect(RC::kOk);

        slots_rdma_buffers.emplace(
            slot_handle,
            KVBlockHandle(remote_kvblock_addr, (KVBlock *) rdma_buffer.buffer));
    }

    rdma_adpt_->commit(trace).expect(RC::kOk);
    trace.pin("read kvblock");

    end_read_kvblock();
    trace.pin("end read kvblock");

    RetCode rc = kNotFound;
    for (const auto &[slot_handle, kvblock_handle] : slots_rdma_buffers)
    {
        RetCode this_is_real_match =
            is_real_match(kvblock_handle.buffer_addr(), key, trace);
        if (unlikely(conf_.force_kvblock_to_match))
        {
            this_is_real_match = RC::kOk;
        }

        if (this_is_real_match != kOk)
        {
            if constexpr (kEnableHistory)
            {
                history.current().add(Record{.ma = kQuery,
                                             .ua = kReadMismatch,
                                             .raddr = slot_handle.remote_addr(),
                                             .key = *(HashKey *) key.data(),
                                             .slot_val = slot_handle.fp()});
            }
            continue;
        }
        // okay, it is the real match
        // has_real_match = true;
        if constexpr (kEnableHistory)
        {
            history.current().add(Record{.ma = kQuery,
                                         .ua = kRead,
                                         .raddr = slot_handle.remote_addr(),
                                         .key = *(HashKey *) key.data(),
                                         .slot_val = slot_handle.fp()});
        }

        // if (trace.enabled())
        // {
        //     auto expected_val = trace.get("expected");
        //     auto *kvblock = kvblock_handle.buffer_addr();
        //     std::string_view got_v(kvblock->buf + kvblock->key_len,
        //                            kvblock->value_len);
        //     if (!got_v.starts_with(expected_val))
        //     {
        //         LOG(ERROR) << "!!! I got you here!!!";
        //     }
        // }

        rc = func(key, slot_handle, kvblock_handle, trace);

        CHECK_NE(rc, kNotFound)
            << "Make no sense to return kNotFound: already found for you.";
        if (unlikely(rc == kRdmaProtectionErr))
        {
            return rc;
        }
    }

    // if (trace.enabled())
    // {
    //     std::string expect_value = trace.get("expected");
    //     if (rc == kNotFound)
    //     {
    //         bool report = !expect_value.empty();
    //         if (report)
    //         {
    //             LOG(ERROR) << "** expect " << PRE(expect_value) << ", got "
    //                        << PRE(rc);
    //             for (const auto &[slot_handle, kvblock_handle] :
    //                  slots_rdma_buffers)
    //             {
    //                 CHECK_EQ(slot_handle.ptr(),
    //                 kvblock_handle.remote_addr()); LOG(INFO)
    //                     << "[Testing] slot_handle at " << slot_handle.ptr()
    //                     << ", pointed to kv_block at "
    //                     << kvblock_handle.remote_addr();
    //                 // RetCode this_is_real_match =
    //                 //     is_real_match(kvblock_handle.buffer_addr(), key,
    //                 //     trace);
    //                 auto *kvblock = kvblock_handle.buffer_addr();
    //                 if (kvblock->key_len != key.size())
    //                 {
    //                     LOG(INFO) << "Key len mismatch: kvblock->key_len: "
    //                               << kvblock->key_len
    //                               << ", key.size(): " << key.size();
    //                 }
    //                 else if (memcmp(key.data(), kvblock->buf, key.size()) !=
    //                 0)
    //                 {
    //                     LOG(INFO)
    //                         << "Key mismatch: hash key: "
    //                         << *(HashKey *) key.data()
    //                         << "remote_key: " << *(HashKey *) kvblock->buf
    //                         << std::endl
    //                         << "key: "
    //                         << util::InlinedHexdump(key.data(), key.size())
    //                         << std::endl
    //                         << "remote key: "
    //                         << util::InlinedHexdump(kvblock->buf,
    //                                                 kvblock->key_len)
    //                         << std::endl
    //                         << "kvblock: " << std::endl
    //                         << util::Hexdump(kvblock->buf, 128);
    //                 }
    //                 else
    //                 {
    //                     LOG(INFO)
    //                         << "Exact match. key: " << key.to_sv()
    //                         << ", actual_val: "
    //                         << std::string_view(kvblock->buf +
    //                         kvblock->key_len,
    //                                             kvblock->value_len)
    //                         << std::endl
    //                         << util::Hexdump(kvblock->buf, 128);
    //                 }
    //             }
    //         }
    //     }
    // }

    return rc;
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::get(const BufferView &key,
                                            BufferView &value,
                                            util::TraceView trace)
{
    if (unlikely(!inited_))
    {
        init(trace);
    }
    DCHECK(inited_);
    auto hash = hash_impl(key.data(), key.size());
    auto m = hash_m(hash);
    auto fp = hash_fp(hash);
    auto cached_gd = gd();
    auto rounded_m = round_to_bits(m, cached_gd);
    auto [h1, h2] = hash_h1_h2(hash);

    DVLOG(V) << "[race] GET key "
             << util::InlinedHexdump(key.data(), key.size()) << ", got hash "
             << pre_hash(hash) << ", m: " << m << ", rounded to " << rounded_m
             << " by cached_gd: " << cached_gd << ". fp: " << pre_fp(fp);
    auto st = subtable_handle(rounded_m);
    trace.pin("get subtable");

    // get the two combined buckets the same time
    // validate staleness at the same time
    auto cbs = st.get_two_combined_bucket_handle(h1, h2, *rdma_adpt_);
    rdma_adpt_->commit(trace).expect(RC::kOk);
    trace.pin("get combined bucket");

    std::unordered_set<SlotHandle> slot_handles;
    auto rc = cbs.locate(fp, cached_ld(rounded_m), m, slot_handles, trace);
    if constexpr (debug())
    {
        for (const auto &slot_handle : slot_handles)
        {
            CHECK_EQ(slot_handle.fp(), fp)
                << "** internal inconsistent: slot_handle.fp(): "
                << (void *) (uint64_t) slot_handle.fp() << " vs "
                << (void *) (uint64_t) fp;
        }
    }
    if (rc == kCacheStale && auto_update_dir_)
    {
        // update cache and retry
        update_directory_cache(trace).expect(RC::kOk);
        return get(key, value, trace);
    }
    DLOG_IF(INFO, config::kEnableDebug)
        << "[race][trace] GET from subtable[" << rounded_m << "] from hash "
        << pre_hash(hash) << " and gd " << cached_gd
        << ". Possible match nr: " << slot_handles.size() << ". "
        << util::pre(trace.kv());

    return get_from_slot_views(slot_handles, key, value, trace);
}

template <size_t E, size_t B, size_t S>
void RaceHashingHandleImpl<E, B, S>::print_latest_meta_image(
    util::TraceView trace)
{
    auto rdma_buf = rdma_adpt_->get_rdma_buffer(sizeof(MetaT));
    DCHECK_GE(rdma_buf.size, sizeof(MetaT));
    auto &dir_mem_handle = get_directory_mem_handle();
    rdma_adpt_
        ->rdma_read(rdma_buf.buffer,
                    table_meta_addr_,
                    sizeof(MetaT),
                    0 /* flag */,
                    dir_mem_handle)
        .expect(RC::kOk);
    rdma_adpt_->commit(trace).expect(RC::kOk);
    auto &meta = *(MetaT *) rdma_buf.buffer;
    DLOG_IF(INFO, config::kEnableExpandDebug)
        << "[race][expand][result][debug] The latest remote meta: " << meta
        << ". trace: " << util::pre(trace.kv());
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::update_if_exists(
    size_t subtable_idx,
    const std::unordered_set<SlotHandle> &slot_handles,
    const Key &key,
    SlotView new_slot,
    util::TraceView trace)
{
    auto &st_mem_handle = get_subtable_mem_handle(subtable_idx);
    auto f = [&rdma_adpt = *rdma_adpt_.get(), &st_mem_handle, new_slot, this](
                 const Key &key,
                 SlotHandle slot_handle,
                 KVBlockHandle kvblock_handle,
                 util::TraceView trace) -> RetCode {
        uint64_t expect_val = slot_handle.val();
        auto rdma_buf = rdma_adpt.get_rdma_buffer(8);
        DCHECK_GE(rdma_buf.size, 8);

        if (avis::Config::ins().enable_ptl())
        {
            auto ptl = rdma_adpt.ptl();
            if (ptl)
            {
                ptl->record_free(slot_handle.ptr());
            }
        }
        if (avis::Config::ins().enable_bp())
        {
            auto bp_buf = rdma_adpt.get_rdma_buffer(8);
            auto value = slot_handle.remote_addr().val;
            memcpy(bp_buf.buffer, &value, 8);
            auto bp_gaddr = new_slot.ptr() - 8;

            LOG_IF(INFO, avis::Config::kReportBP)
                << "[BP] writing to " << bp_gaddr << " with val "
                << (void *) value;

            rdma_adpt.ptl()->bp_metric().write(8);
            rdma_adpt
                .rdma_write(
                    bp_gaddr, bp_buf.buffer, 8, 0 /* flag */, st_mem_handle)
                .expect(RC::kOk);
        }
        CHECK_EQ(rdma_adpt.rdma_cas(slot_handle.remote_addr(),
                                    expect_val,
                                    new_slot.val(),
                                    rdma_buf.buffer,
                                    0 /* flag */,
                                    st_mem_handle),
                 kOk);
        rdma_adpt.commit(trace).expect(RC::kOk);
        trace.pin("CAS");
        bool success = memcmp(rdma_buf.buffer, (char *) &expect_val, 8) == 0;
        expect_val = *(uint64_t *) rdma_buf.buffer;
        SlotView expect_slot(expect_val);
        if (success)
        {
            if constexpr (kEnableHistory)
            {
                history.current().add(
                    Record{.ma = kUpsert,
                           .ua = kCAS,
                           .raddr = slot_handle.remote_addr(),
                           .raddr2 = GlobalAddress((void *) new_slot.val()),
                           .key = *(HashKey *) key.data()});
            }
            if (expect_slot.empty())
            {
                DVLOG(V) << "[race][subtable] do_update SUCC: update into "
                            "an empty slot. New_slot "
                         << new_slot;
            }
            else
            {
                auto kvblock_remote_mem = expect_slot.ptr();
                DVLOG(V) << "[race][subtable] do_update SUCC: for slot "
                            "with kvblock_handle:"
                         << kvblock_handle << ". New_slot " << new_slot;
                DCHECK_EQ(expect_slot.ptr(), kvblock_handle.remote_addr())
                    << "** internal inconsistency?";

                do_free_kvblock(kvblock_remote_mem,
                                kvblock_handle.total_size());
                trace.pin("free block");
            }
            DLOG_IF(INFO, config::kEnableDebug)
                << "[race][subtable] slot " << slot_handle << " update to "
                << new_slot << util::pre(trace.kv());
            trace.pin("OK");
            return kOk;
        }
        DVLOG(V) << "[race][subtable] do_update FAILED: new_slot " << new_slot;
        DLOG_IF(INFO, config::kEnableDebug)
            << "[race][trace][subtable] do_update FAILED: cas failed. slot "
            << slot_handle << util::pre(trace.kv());
        trace.pin("Retry");
        return kRetry;
    };

    return for_the_real_match_do(slot_handles, key, f, trace);
}

template <size_t E, size_t B, size_t S>
RetCode RaceHashingHandleImpl<E, B, S>::is_real_match(KVBlock *kvblock,
                                                      const Key &key,
                                                      util::TraceView trace)
{
    std::ignore = trace;
    if (kvblock->key_len != key.size())
    {
        // DVLOG(V) << "[race][subtable] slot_real_match FAILED: key "
        //          << pre(key) << " miss: key len mismatch";
        DLOG_IF(INFO, config::kEnableLocateDebug)
            << "[race][stable] is_real_match FAILED: key len " << key.size()
            << " mismatch with block->key_len: " << kvblock->key_len << ". "
            << util::pre(trace.kv());
        return kNotFound;
    }
    if (memcmp(key.data(), kvblock->buf, key.size()) != 0)
    {
        // DVLOG(V) << "[race][subtable] slot_real_match FAILED: key "
        //          << pre(key) << " miss: key content mismatch";
        DLOG_IF(INFO, config::kEnableLocateDebug)
            << "[race][stable] is_real_match FAILED: key content mismatch. "
               "expect: "
            << util::InlinedHexdump(key.data(), key.size())
            << ", got: " << std::string((const char *) kvblock->buf, key.size())
            << util::pre(trace.kv());
        return kNotFound;
    }
    // DVLOG(V) << "[race][subtable] slot_real_match SUCCEED: key "
    //          << pre(key);
    DLOG_IF(INFO, config::kEnableLocateDebug)
        << "[race][stable] is_real_match SUCC. " << util::pre(trace.kv());
    return kOk;
}

}  // namespace patronus::hash