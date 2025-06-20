#pragma once
#include <cinttypes>

#include "./config.h"
#include "CoroContext.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "Metrics.h"

namespace avis
{
enum PTL_Dir
{
    kBuddyToSlab,
    kSlabToUser,
    kFree,
};
struct PTLRecord
{
    PTL_Dir dir;
    GlobalAddress gaddr;
    uint64_t desc;
};

inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] ::avis::PTL_Dir e)
{
    switch (e)
    {
    case ::avis::kBuddyToSlab:
        os << "kBuddyToSlab";
        break;
    case ::avis::kSlabToUser:
        os << "kSlabToUser";
        break;
    case ::avis::kFree:
        os << "kFree";
        break;
    default:
        os << "Unknown ::avis::PTL_Dir(" << (int) e << ")";
    }
    return os;
}
inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::avis::PTLRecord &v)
{
    os << "{PTL ";
    os << "dir: " << util::pre(v.dir);
    os << ", gaddr: " << util::pre(v.gaddr);
    os << ", desc: " << util::pre(v.desc);
    os << "}";
    return os;
}

class PTL
{
public:
    constexpr static bool kReport = Config::kReportPTL;

    PTL(DSM::pointer dsm, size_t server_nid, CoroContext *ctx)
        : dsm_(dsm), server_nid_(server_nid), ctx_(ctx)
    {
        ptl_size_ = 128;
        ptl_area_ = dsm_->alloc_from(ptl_size_, server_nid_);
        ptl_buf_ = dsm_->get_rdma_buffer(ptl_size_);
    }
    void set_ctx(CoroContext *ctx)
    {
        ctx_ = ctx;
    }
    void record_buddy_to_bitmap(uint32_t buddy_id, uint32_t node_id)
    {
        if (Config::ins().enable_ptl())
        {
            PTLRecord *ptl = (PTLRecord *) ptl_buf_.buffer;
            ptl->dir = PTL_Dir::kBuddyToSlab;
            ptl->desc = (((uint64_t) buddy_id) << 32) + node_id;

            LOG_IF(INFO, kReport) << "[PTL] writing buddy " << *ptl;
            dsm_->prepare_write(
                ptl_buf_.buffer, ptl_area_, sizeof(PTLRecord), false, ctx_);
            rm_.write(sizeof(PTLRecord));
            // NOTE: no need to commit: we can carry together the next request
            // because W-W is ordering
        }
    }
    void record_free(GlobalAddress addr)
    {
        if (Config::ins().enable_ptl())
        {
            PTLRecord *ptl = (PTLRecord *) ptl_buf_.buffer;
            ptl->dir = PTL_Dir::kFree;
            ptl->gaddr = addr;
            LOG_IF(INFO, kReport) << "[PTL] before free " << *ptl;
            dsm_->prepare_write(
                ptl_buf_.buffer, ptl_area_, sizeof(PTLRecord), false, ctx_);
            rm_.write(sizeof(PTLRecord));

            // NOTE: no need to commit: we can carry together the next request
            // because W-W is ordering
        }
    }
    void log_async(void *buf, size_t size, Buffer &rdma_buf)
    {
        DCHECK_GE(rdma_buf.size, size);
        memcpy(rdma_buf.buffer, buf, size);
        DCHECK_LE(size, ptl_size_);
        dsm_->prepare_write(rdma_buf.buffer, ptl_area_, size, false, ctx_);
    }
    void log_sync(void *buf, size_t size)
    {
        auto rdma_buf = dsm_->get_rdma_buffer(size);
        log_async(buf, size, rdma_buf);
        dsm_->commit(ctx_);
        dsm_->put_rdma_buffer(std::move(rdma_buf));
    }

    void record_bitmap_to_slab(GlobalAddress bitmap_raddr, uint64_t block_id)
    {
        if (Config::ins().enable_ptl())
        {
            PTLRecord *ptl = (PTLRecord *) ptl_buf_.buffer;
            ptl->dir = PTL_Dir::kSlabToUser;
            ptl->desc = block_id;
            ptl->gaddr = bitmap_raddr;

            LOG_IF(INFO, kReport) << "[PTL] writing bitmap " << *ptl;
            dsm_->prepare_write(
                ptl_buf_.buffer, ptl_area_, sizeof(PTLRecord), false, ctx_);
            rm_.write(sizeof(PTLRecord));
            // NOTE: no need to commit: we can carry together the next request
            // because W-W is ordering
        }
    }
    auto &metric()
    {
        return rm_;
    }
    auto &bp_metric()
    {
        return bp_rm_;
    }
    ~PTL()
    {
        if (!ptl_area_.is_null())
        {
            dsm_->free(ptl_area_, ptl_size_);
        }
        if (ptl_buf_.buffer)
        {
            dsm_->put_rdma_buffer(std::move(ptl_buf_));
        }
    }

    GlobalAddress ptl_area() const
    {
        return ptl_area_;
    }
    size_t ptl_size() const
    {
        return ptl_size_;
    }
    size_t server_nid() const
    {
        return server_nid_;
    }

private:
    DSM::pointer dsm_;
    size_t server_nid_;
    CoroContext *ctx_;

    size_t ptl_size_{};
    Buffer ptl_buf_{};
    GlobalAddress ptl_area_{};

    RemoteMetrics rm_;
    RemoteMetrics bp_rm_;
};
}  // namespace avis