#pragma once
#include "./debug.h"
#include "./handle.h"
#include "CoroContext.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "Metrics.h"
#include "patronus/Type.h"
#include "util/CRTP.h"
#include "util/Coro.h"
#include "util/Debug.h"
#include "util/IRdmaAdaptor.h"
#include "util/PreUtil.h"
#include "util/Tracer.h"
#include "util/stacktrace.h"
#include "util/thread_id.h"

namespace avis
{
class AvisAdaptor : public IRdmaAdaptor, public util::MakeShared<AvisAdaptor>
{
public:
    constexpr static size_t kWarningOngoingRdmaBuf = 16;
    constexpr static bool kReportMemAlloc = false;
    constexpr static bool kEnableAllocHistory = false;
    constexpr static bool kReport = false;
    /**
     * For client, provide a handle.
     * For server, provide a manager.
     */
    AvisAdaptor(AvisHandle::Pointer handle, size_t node_id)
        : handle_(std::move(handle)),
          dsm_(handle_ ? handle_->dsm() : nullptr),
          node_id_(node_id)
    {
    }
    AvisAdaptor(DSM::pointer dsm) : handle_(nullptr), dsm_(dsm)
    {
    }
    void set_ctx(CoroContext *ctx)
    {
        handle_->set_ctx(ctx);
    }
    GlobalAddress remote_alloc(size_t size, hint_t) override
    {
        DCHECK_GT(size, 0);
        GlobalAddress raddr;
        if (Config::ins().enable_bp())
        {
            raddr = handle_->alloc(size + 8);
            raddr.offset += 8;
        }
        else
        {
            raddr = handle_->alloc(size);
        }
        if (!raddr.is_null())
        {
            DCHECK_EQ(raddr.nodeID, node_id_)
                << "** vialation: expect allocation from node_id: " << node_id_;
        }
        raddr.nodeID = 0;

        if (likely(!raddr.is_null()))
        {
            trace_alloc(raddr, size);
        }
        LOG_IF(INFO, kReport) << "[avis] alloc " << raddr << ", size " << size;
        return raddr;
    }
    void remote_free(GlobalAddress raddr, size_t size, hint_t) override
    {
        if (unlikely(raddr.is_null()))
        {
            return;
        }
        DCHECK_GT(size, 0);
        raddr.nodeID = node_id_;
        if (likely(!raddr.is_null()))
        {
            trace_free(raddr, size);
        }
        LOG_IF(INFO, kReport && !raddr.is_null()) << "[avis] free " << raddr;
        if (Config::ins().enable_bp())
        {
            handle_->free(raddr - 8, size + 8);
        }
        else
        {
            handle_->free(raddr, size);
        }
    }
    std::shared_ptr<avis::PTL> ptl() override
    {
        return handle_->ptl();
    }
    auto partition_metric() const
    {
        return handle_->partition_metric();
    }
    void trace_free(GlobalAddress raddr, size_t size)
    {
        am_.record_dealloc(size);
        LOG_IF(INFO, kReportMemAlloc) << "[AvisAdaptor] free " << size;

        if constexpr (kEnableAllocHistory)
        {
            history.current().add(
                HisRecord{.is_alloc = false,
                          .raddr = raddr,
                          .size = size,
                          .tid = (int) util::get_thread_id(),
                          .cid = handle_->coro_ctx()
                                     ? (int) handle_->coro_ctx()->coro_id()
                                     : (int) kNotACoro});
        }
    }
    void trace_alloc(GlobalAddress raddr, size_t size)
    {
        am_.record_alloc(size);
        LOG_IF(INFO, kReportMemAlloc) << "[AvisAdaptor] allocate " << size;
        if constexpr (kEnableAllocHistory)
        {
            history.current().add(
                HisRecord{.is_alloc = true,
                          .raddr = raddr,
                          .size = size,
                          .tid = (int) util::get_thread_id(),
                          .cid = handle_->coro_ctx()
                                     ? (int) handle_->coro_ctx()->coro_id()
                                     : (int) kNotACoro});
        }
    }
    auto usage() const
    {
        return am_;
    }
    Buffer get_rdma_buffer(size_t size) override
    {
        auto buf = dsm_->get_rdma_buffer(size);
        allocated_bytes_ += size;
        allocated_nr_++;
        // DCHECK_NE(buf.size, 0) << "** possibly run out of buffer: "
        ongoing_rdma_bufs_.emplace_back(std::move(buf));
        if (unlikely(ongoing_rdma_bufs_.size() == kWarningOngoingRdmaBuf))
        {
            // LOG(WARNING) << "WARNING: too many ongoing buffers";
        }
        auto ret = ongoing_rdma_bufs_.back().clone();
        if (unlikely(ret.size == 0))
        {
            LOG(FATAL) << "[debug] run out of memory: allocated "
                       << util::pre_byte(allocated_bytes_) << " for "
                       << allocated_nr_ << " times";
        }
        return ret;
    }
    void put_all_rdma_buffer() override
    {
        for (auto &&buf : ongoing_rdma_bufs_)
        {
            dsm_->put_rdma_buffer(std::move(buf));
        }
        ongoing_rdma_bufs_.clear();
        allocated_bytes_ = 0;
        allocated_nr_ = 0;
    }
    /**
     * get_manager_allocator returns an allocator that returns memory that is
     * - accessible: support memcpy, memset, etc
     * - translatable: can be returned to avis-understandable remote memory
     */
    auto get_manager_allocator()
    {
        return dsm_->get_dsm_allocator();
    }

    RetCode rdma_read(void *rdma_buf,
                      GlobalAddress raddr,
                      size_t size,
                      flag_t,
                      RemoteMemHandle &) override
    {
        raddr.nodeID = node_id_;
        handle_->prepare_read((char *) rdma_buf, raddr, size);
        rm_.read(size);
        return RC::kOk;
    }
    RetCode rdma_write(GlobalAddress raddr,
                       void *rdma_buf,
                       size_t size,
                       flag_t,
                       RemoteMemHandle &) override
    {
        raddr.nodeID = node_id_;
        handle_->prepare_write((char *) rdma_buf, raddr, size);
        rm_.write(size);
        return RC::kOk;
    }
    RetCode rdma_cas(GlobalAddress raddr,
                     uint64_t expect,
                     uint64_t desired,
                     void *rdma_buf,
                     flag_t,
                     RemoteMemHandle &) override
    {
        raddr.nodeID = node_id_;
        uint64_t mask = 0xffffffffffffffff;
        handle_->prepare_cas(raddr, 8, expect, mask, desired, mask, rdma_buf);
        rm_.cas(8, true);

        return RC::kOk;
    }
    RetCode rdma_faa(GlobalAddress raddr,
                     int64_t value,
                     void *rdma_buf,
                     flag_t,
                     RemoteMemHandle &) override
    {
        raddr.nodeID = node_id_;
        uint64_t field_boundary = 0;
        handle_->prepare_faa(raddr, 8, value, field_boundary, rdma_buf);
        rm_.faa(8);
        return RC::kOk;
    }
    RetCode commit(util::TraceView trace = util::nulltrace) override
    {
        handle_->commit(trace);
        return RC::kOk;
    }
    GlobalAddress to_exposed_raddr(void *addr) override
    {
        auto gaddr = dsm_->to_exposed_gaddr(addr);
        return gaddr;
    }
    void *from_exposed_raddr(GlobalAddress gaddr) override
    {
        auto [nid, addr] = dsm_->explain_gaddr(gaddr);
        DCHECK_EQ(nid, dsm_->get_node_id());
        return addr;
    }
    std::pair<AllocMetrics, RemoteMetrics> metrics() const
    {
        return {am_, rm_};
    }

    void reg_overwrite_allocator(uint64_t, ::mem::IAllocator::pointer) override
    {
        LOG(FATAL) << "Unsupported operation";
    }

    RemoteMemHandle acquire_perm(GlobalAddress raddr,
                                 hint_t,
                                 size_t size,
                                 std::chrono::nanoseconds,
                                 flag_t) override
    {
        RemoteMemHandle ret(
            raddr, size, patronus::AcquireRequestStatus::kSuccess);
        return ret;
    }
    RetCode extend(RemoteMemHandle &, std::chrono::nanoseconds) override
    {
        return RC::kOk;
    }
    void relinquish_perm(RemoteMemHandle &h, hint_t, flag_t) override
    {
        h.set_invalid();
    }
    void debug_explain_raddr(GlobalAddress) override
    {
        LOG(FATAL) << "deprecated";
    }
    ~AvisAdaptor()
    {
        put_all_rdma_buffer();
    }
    AvisHandle::Pointer get_handle()
    {
        return handle_;
    }

private:
    AvisHandle::Pointer handle_;
    DSM::pointer dsm_;
    size_t node_id_;

    std::vector<Buffer> ongoing_rdma_bufs_;
    size_t allocated_bytes_{};
    size_t allocated_nr_{};

    std::unordered_map<GlobalAddress, size_t> debug_;

    AllocMetrics am_;
    RemoteMetrics rm_;
};
}  // namespace avis