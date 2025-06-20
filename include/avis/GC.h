#pragma once
#include "./ptl.h"
#include "Common.h"
#include "GlobalAddress.h"
#include "util/Rand.h"

namespace avis
{
class GC
{
public:
    GC(DSM::pointer dsm, std::shared_ptr<PTL> ptl, size_t leak_size)
        : dsm_(dsm), ptl_(ptl), leak_size_(leak_size)
    {
    }

    void recover(size_t parallel)
    {
        ChronoTimer timer;
        // 1) read PTL area
        auto ptl_size = ptl_->ptl_size();
        auto ptl_area = ptl_->ptl_area();
        auto rdma_buf = dsm_->get_rdma_buffer(ptl_size);
        dsm_->prepare_read(rdma_buf.buffer, ptl_area, ptl_size, false, nullptr);
        dsm_->commit(nullptr);

        auto read_ptl_ns = timer.pin();

        // 2) detect PTL gaddrs
        auto ptl_gaddrs = get_ptl_gaddrs(rdma_buf);

        // 3) recover
        std::vector<Buffer> rdma_bufs;
        rdma_bufs.reserve(ptl_gaddrs.size());
        size_t ongoing_size = 0;
        for (const auto &gaddr : ptl_gaddrs)
        {
            rdma_bufs.emplace_back(dsm_->get_rdma_buffer(8));
            dsm_->prepare_read(
                rdma_bufs.back().buffer, gaddr, 8, false, nullptr);
            ongoing_size++;
            if (ongoing_size >= parallel)
            {
                dsm_->commit(nullptr);
                ongoing_size = 0;
            }
        }
        if (ongoing_size)
        {
            dsm_->commit(nullptr);
            ongoing_size = 0;
        }
        auto recover_ns = timer.pin();

        for (auto &&buf : rdma_bufs)
        {
            dsm_->put_rdma_buffer(std::move(buf));
        }
        dsm_->put_rdma_buffer(std::move(rdma_buf));

        LOG(INFO) << "Total " << util::pre_ns(read_ptl_ns + recover_ns)
                  << " for " << leak_size_ << " buffers with parallel "
                  << parallel
                  << ". Break down: read_ptl_ns: " << util::pre_ns(read_ptl_ns)
                  << ", recover_ns: " << util::pre_ns(recover_ns);
    }

private:
    DSM::pointer dsm_;
    std::shared_ptr<PTL> ptl_;
    size_t leak_size_;

    std::vector<GlobalAddress> get_ptl_gaddrs(const Buffer &)
    {
        constexpr size_t kRange = 2_GB;
        std::vector<GlobalAddress> ret;
        ret.reserve(leak_size_);
        auto server_nid = ptl_->server_nid();
        for (size_t i = 0; i < leak_size_; ++i)
        {
            GlobalAddress gaddr;
            gaddr.nodeID = server_nid;
            gaddr.offset = fast_pseudo_rand_int(1, kRange / 64) * 64;
            ret.emplace_back(gaddr);
        }
        return ret;
    }
};
}  // namespace avis