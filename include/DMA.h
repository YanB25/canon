#pragma once
#include "DSM.h"

class DMAEngine
{
public:
    constexpr static size_t kMaxOp = 128;

    // sge_per_wr: higher is better, set to max allowed, i.e., 16
    DMAEngine(DSM::pointer dsm, size_t sge_per_wr = 16)
        : dsm_(dsm), sge_per_wr_(sge_per_wr)
    {
    }

    void prepare_memcpy(void *source, void *dest, size_t size)
    {
        ibv_qp *qp = nullptr;
        uint32_t node_id = dsm_->get_node_id();
        uint32_t dir_id = dsm_->get_thread_id();
        auto &sge = sges_[sge_idx_];
        sge_idx_++;

        memset(&sge, 0, sizeof(sge));
        sge.addr = (uintptr_t) source;
        sge.length = size;
        sge.lkey = dsm_->get_icon_lkey();
    }

    bool commit(uint64_t wr_id)
    {
        if (unlikely(sge_idx_ == 0))
        {
            return false;
        }

        size_t batch_nr = round_up_div(sge_idx_, sge_per_wr_);
        size_t cur_sge_id = 0;
        for (size_t batch_id = 0; batch_id < batch_nr; ++batch_id)
        {
            bool last = batch_id + 1 == batch_nr;
            size_t batch_size = last ? (sge_idx_ % sge_per_wr_) : sge_per_wr_;

            auto &wr = send_wrs_[wr_idx_];
            wr_idx_++;
            wr.wr_id = wr_id;
            wr.num_sge = batch_size;
            wr.sg_list = &sges_[cur_sge_id];
            wr.opcode = IBV_WR_RDMA_WRITE;
            wr.send_flags = 0;

            cur_sge_id += batch_size;
        }

        reset();
        return true;
    }

    void reset()
    {
        sge_idx_ = 0;
        wr_idx_ = 0;
    }

private:
    DSM::pointer dsm_;
    size_t sge_per_wr_;

    ibv_sge sges_[kMaxOp]{};
    ssize_t sge_idx_{0};
    ibv_send_wr send_wrs_[kMaxOp]{};
    ssize_t wr_idx_{0};
    ibv_send_wr *bad_wr_{nullptr};
};