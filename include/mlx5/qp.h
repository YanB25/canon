#pragma once
#include <iostream>

#include "Buffer.h"
#include "wqe.h"
extern "C"
{
#include "infiniband/mlx5dv.h"
#include "infiniband/verbs.h"
#include "infiniband/verbs_exp.h"
#include "mlx5/libmlx5/mlx5.h"
}

namespace mlx5
{
class SQ
{
public:
    constexpr static size_t kWQEBB_Size = 64;
    SQ(mlx5_wq *sq) : sq_(sq)
    {
    }
    void *buff() const
    {
        return sq_->buff;
    }
    auto wqe_shift() const
    {
        return sq_->wqe_shift;
    }
    void *addr() const
    {
        return buff();
    }
    // I believe it is a *typo*
    // is actually wqebb_cnt
    auto wqebb_cnt() const
    {
        return sq_->wqe_cnt;
    }
    auto buf_size() const
    {
        return wqebb_cnt() << wqe_shift();
    }
    void *wqebb_addr(size_t idx) const
    {
        return (char *) addr() + idx * kWQEBB_Size;
    }
    Buffer buffer() const
    {
        return Buffer(buff(), buf_size());
    }
    // qp->gen_data.sqstart == qp->buf.buf + qp->sq.offset
    int offset() const
    {
        return sq_->offset;
    }
    WQE wqe(size_t idx) const
    {
        void *pos = wqebb_addr(idx);
        return WQE(pos);
    }
    UMR_WQE umr_wqe(size_t idx) const
    {
        void *pos = wqebb_addr(idx);
        return UMR_WQE(pos);
    }
    // max_post denotes the max *ongoing* WQEs
    // i.e., head - tail <= max_post
    // `max_post` is always much smaller than `wqe_cnt`
    auto max_post() const
    {
        return sq_->max_post;
    }
    // head and tail are always increasing

    auto head() const
    {
        return sq_->head;
    }
    auto tail() const
    {
        return sq_->tail;
    }
    auto max_active_wqe_nr() const
    {
        return max_post();
    }
    auto active_wqe_nr() const
    {
        auto t = tail();
        auto h = head();
        DCHECK_GE(h, t);
        return h - t;
    }
    size_t remain_post_nr() const
    {
        auto m = max_active_wqe_nr();
        auto a = active_wqe_nr();
        DCHECK_GE(m, a);
        return m - a;
    }
    bool can_post() const
    {
        return remain_post_nr() > 0;
    }

private:
    mlx5_wq *sq_;
};
inline std::ostream &operator<<(std::ostream &os, const SQ &sq)
{
    os << fmt::format(
        "{{SQ buff: {}, wqe_cnt: {}, shift: {}, max_post: {}, head: {}, "
        "tail: "
        "{} }}",
        sq.buff(),
        sq.wqebb_cnt(),
        sq.wqe_shift(),
        sq.max_post(),
        sq.head(),
        sq.tail());
    return os;
}

class RQ
{
public:
    RQ(mlx5_wq *rq) : rq_(rq)
    {
    }
    void *addr()
    {
        return rq_->buff;
    }

private:
    mlx5_wq *rq_;
};

class QP
{
public:
    QP(ibv_qp *qp);

    ibv_qp *ibqp()
    {
        return ibqp_;
    }
    mlx5_qp *mqp()
    {
        return mqp_;
    }

    SQ sq()
    {
        return SQ(&(mqp_->sq));
    }
    RQ rq()
    {
        return RQ(&(mqp_->rq));
    }
    // for sq, the lower 32bit of doorbell record denotes the number of WQEBB
    // (64B) wrap around at 0xFFFF
    uint32_t read_send_db() const
    {
        auto val = *send_db();
        return ntohl(val);
    }
    volatile uint32_t *send_db()
    {
        return mqp_->gen_data.db + MLX5_SND_DBR;
    }
    const volatile uint32_t *send_db() const
    {
        return mqp_->gen_data.db + MLX5_SND_DBR;
    }
    // for rq, the lower 32 bit of doorbell record denotes the number of WQE
    // wrap around at 0xFFFF
    volatile uint32_t *recv_db()
    {
        return mqp_->gen_data.db + MLX5_RCV_DBR;
    }
    const volatile uint32_t *recv_db() const
    {
        return mqp_->gen_data.db + MLX5_RCV_DBR;
    }
    // scur_post denotes the count of posted WQEBB (64B)
    // always increasing
    auto scur_post() const
    {
        return mqp_->gen_data.scur_post;
    }
    // NOTE: the *last* API points to the *next* position to be posted to
    // i.e., the returned slot is currently empty (available).
    size_t last_wqebb_idx() const
    {
        auto ret = scur_post() & (mqp_->sq.wqe_cnt - 1);
        // LOG(WARNING) << "DEBUG: scur_post: " << mqp_->gen_data.scur_post
        //              << ", wqe_cnt: " << mqp_->sq.wqe_cnt
        //              << ", mask: " << util::pre_hex(mqp_->sq.wqe_cnt - 1)
        //              << ", " << PRE(ret);
        return ret;
    }
    void *last_wqe_addr() const
    {
        return last_wqebb_addr();
    }
    void *last_wqebb_addr() const
    {
        auto idx = last_wqebb_idx();
        constexpr static size_t MLX5_SEND_WQEBB_SHIFT = MLX5_SEND_WQE_SHIFT;
        return (char *) sq_start() + (idx << MLX5_SEND_WQEBB_SHIFT);
    }
    WQE last_wqe()
    {
        return WQE(last_wqe_addr());
    }
    UMR_WQE last_umr_wqe()
    {
        return UMR_WQE(last_wqe_addr());
    }
    CR_WQE last_cr_wqe()
    {
        return CR_WQE(last_wqe_addr());
    }
    void *sq_start() const
    {
        return mqp_->gen_data.sqstart;
    }
    void *rq_start()
    {
        return mqp_->rq.buff;
    }

private:
    ibv_qp *ibqp_{};
    mlx5_qp *mqp_{};
};

}  // namespace mlx5