#pragma once
#include "DSM.h"
#include "Metrics.h"
#include "util/CRTP.h"
#include "util/IRdmaAdaptor.h"
#include "util/PreUtil.h"
#include "util/Tracer.h"

class DSMAdaptor : public IRdmaAdaptor, public util::MakeShared<DSMAdaptor>
{
public:
    constexpr static size_t kWarningOngoingRdmaBuf = 16;
    DSMAdaptor(uint16_t node_id, DSM::pointer dsm, CoroContext *ctx)
        : node_id_(node_id), dsm_(dsm), ctx_(ctx)
    {
    }
    GlobalAddress remote_alloc(size_t size, hint_t) override
    {
        GlobalAddress ret = dsm_->alloc_from(size, node_id_);
        if (unlikely(ret.is_null()))
        {
            LOG(FATAL) << "[DSMAdaptor] ** failed to allocate " << PRE(size)
                       << ": run out of memory: " << dsm_->dsm_usage()
                       << ", self: " << dsm_->dsm_self_usage();
        }
        ret.nodeID = 0;
        u_.record_alloc(size);
        return ret;
    }
    void remote_free(GlobalAddress gaddr, size_t size, hint_t) override
    {
        gaddr.nodeID = node_id_;
        dsm_->free(gaddr, size);
        if (likely(!gaddr.is_null()))
        {
            u_.record_dealloc(size);
        }
    }
    Buffer get_rdma_buffer(size_t size) override
    {
        auto buf = dsm_->get_rdma_buffer(size);
        if (unlikely(buf.size == 0))
        {
            LOG(FATAL) << "[debug] run out of buffer: total "
                       << util::pre_byte(allocated_bytes_) << " for "
                       << allocated_nr_ << " times";
        }
        ongoing_rdma_bufs_.emplace_back(std::move(buf));
        if (unlikely(ongoing_rdma_bufs_.size() == kWarningOngoingRdmaBuf))
        {
            LOG(WARNING) << "WARNING: too many ongoing buffers";
        }
        return ongoing_rdma_bufs_.back().clone();
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

    RetCode rdma_read(void *rdma_buf,
                      GlobalAddress gaddr,
                      size_t size,
                      flag_t,
                      RemoteMemHandle &) override
    {
        gaddr.nodeID = node_id_;
        dsm_->prepare_read(
            (char *) rdma_buf, gaddr, size, false /* on chip */, ctx_);
        return RC::kOk;
    }
    RetCode rdma_write(GlobalAddress gaddr,
                       void *rdma_buf,
                       size_t size,
                       flag_t,
                       RemoteMemHandle &) override
    {
        gaddr.nodeID = node_id_;
        dsm_->prepare_write(
            (char *) rdma_buf, gaddr, size, false /* on chip */, ctx_);
        return RC::kOk;
    }
    RetCode rdma_cas(GlobalAddress gaddr,
                     uint64_t expect,
                     uint64_t desired,
                     void *rdma_buf,
                     flag_t,
                     RemoteMemHandle &) override
    {
        gaddr.nodeID = node_id_;
        dsm_->prepare_cas(gaddr,
                          8,
                          expect,
                          0xffffffffffffffff /* mask */,
                          desired,
                          0xffffffffffffffff /* mask */,
                          (uint64_t *) rdma_buf,
                          false /* on chip */,
                          ctx_);
        return RC::kOk;
    }
    RetCode rdma_faa(GlobalAddress gaddr,
                     int64_t value,
                     void *rdma_buf,
                     flag_t,
                     RemoteMemHandle &) override
    {
        gaddr.nodeID = node_id_;
        dsm_->prepare_faa(gaddr,
                          sizeof(uint64_t),
                          value,
                          0 /* field boundary */,
                          (uint64_t *) rdma_buf,
                          false /* on chip */,
                          ctx_);
        return RC::kOk;
    }
    RetCode commit(util::TraceView trace = util::nulltrace) override
    {
        dsm_->commit(ctx_, trace);
        return RC::kOk;
    }
    GlobalAddress to_exposed_raddr(void *addr) override
    {
        auto base = dsm_->remote_info()[node_id_].dsmBase;
        GlobalAddress ret;
        ret.nodeID = node_id_;
        ret.offset = (uint64_t) addr - base;
        LOG(INFO) << "to_exposed_gaddr: " << PRE(addr) << " => " << PRE(ret)
                  << " due to base " << base;
        return ret;
    }
    void *from_exposed_raddr(GlobalAddress gaddr) override
    {
        auto [_, addr] = dsm_->explain_gaddr(gaddr);
        return addr;
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
    ~DSMAdaptor()
    {
        put_all_rdma_buffer();
    }

private:
    uint16_t node_id_;
    DSM::pointer dsm_;
    CoroContext *ctx_;

    std::vector<Buffer> ongoing_rdma_bufs_;
    size_t allocated_bytes_{};
    size_t allocated_nr_{};

    AllocMetrics u_;
};