#pragma once

#include <atomic>
#include <cinttypes>
#include <climits>
#include <iostream>

#include "glog/logging.h"
#include "util/Pre.h"
extern "C"
{
#include "infiniband/mlx5dv.h"
}
#include "infiniband/verbs.h"
#include "infiniband/verbs_exp.h"

#ifdef __has_builtin
#if __has_builtin(__builtin_bswap64)
#define HAS_BUILTIN_BSWAP64 1
#endif
#endif

#ifndef HAS_BUILTIN_BSWAP64
#define HAS_BUILTIN_BSWAP64 0
#endif

inline uint64_t bswap64(uint64_t val)
{
    if constexpr (HAS_BUILTIN_BSWAP64)
    {
        return __builtin_bswap64(val);
    }
    else
    {
        uint32_t high_part = htonl((uint32_t) (val >> 32));
        uint32_t low_part = htonl((uint32_t) (val & 0xFFFFFFFFLL));
        return ((uint64_t) low_part << 32) | high_part;
    }
}

inline uint64_t htonll(uint64_t host_order_64)
{
    int num = 42;
    if (*(char *) &num == 42)
    {
        // The system is little-endian, so we need to swap the bytes.
        return bswap64(host_order_64);
    }
    else
    {
        // The system is big-endian, so no byte swapping is needed.
        return host_order_64;
    }
}

inline uint64_t ntohll(uint64_t network_order_64)
{
    return htonll(
        network_order_64);  // htonll and ntohll are identical functions.
}

namespace mlx5
{
using wqe_ctrl_seg = mlx5_wqe_ctrl_seg;

class WQE_CtrlSeg
{
public:
    WQE_CtrlSeg(void *seg) : seg_((const wqe_ctrl_seg *) seg)
    {
    }
    uint16_t idx() const
    {
        return (opmod_idx_opcode() >> 8) & (UINT_MAX);
    }
    // It should be opcode from MLX5
    uint8_t opcode() const
    {
        return opmod_idx_opcode() & USHRT_MAX;
    }
    uint8_t signature() const
    {
        return seg_->signature;
    }
    uint8_t fm_ce_se() const
    {
        return seg_->fm_ce_se;
    }
    uint32_t imm() const
    {
        return seg_->imm;
    }
    uint32_t opmod_idx_opcode() const
    {
        return ntohl(seg_->opmod_idx_opcode);
    }
    uint32_t qpn() const
    {
        return qpn_ds() >> 8;
    }
    uint16_t ds() const
    {
        return qpn_ds() & 0xff;
    }
    uint32_t qpn_ds() const
    {
        return ntohl(seg_->qpn_ds);
    }

private:
    const wqe_ctrl_seg *seg_{};
};
inline std::ostream &operator<<(std::ostream &os, const WQE_CtrlSeg &seg)
{
    os << "{wqe_ctrl_seg op: " << (int) seg.opcode() << ", idx: " << seg.idx()
       << ", imm: " << seg.imm() << ", qpn: " << seg.qpn()
       << ", ds: " << seg.ds() << "}";
    return os;
}

using wqe_data_seg = mlx5_wqe_data_seg;

class WQE_DataSeg
{
public:
    WQE_DataSeg(const void *seg) : seg_((const wqe_data_seg *) seg)
    {
    }
    uint32_t byte_count() const
    {
        return ntohl(seg_->byte_count);
    }
    uint32_t lkey() const
    {
        return ntohl(seg_->lkey);
    }
    uint64_t addr() const
    {
        return ntohll(seg_->addr);
    }

private:
    const wqe_data_seg *seg_;
};

inline std::ostream &operator<<(std::ostream &os, const WQE_DataSeg &seg)
{
    os << "{wqe_data_seg addr: " << (void *) seg.addr()
       << ", byte_count: " << seg.byte_count() << ", lkey: " << seg.lkey()
       << "}";
    return os;
}

using wqe_raddr_seg = mlx5_wqe_raddr_seg;

class WQE_RaddrSeg
{
public:
    WQE_RaddrSeg(void *seg) : seg_((const wqe_raddr_seg *) seg)
    {
    }
    uint64_t raddr() const
    {
        return ntohll(seg_->raddr);
    }
    uint32_t rkey() const
    {
        return ntohl(seg_->rkey);
    }

private:
    const wqe_raddr_seg *seg_;
};

inline std::ostream &operator<<(std::ostream &os, const WQE_RaddrSeg &seg)
{
    os << "{wqe_raddr_seg raddr: " << (void *) seg.raddr()
       << ", rkey: " << seg.rkey() << "}";
    return os;
}

class WQE_Segments
{
public:
    WQE_Segments(void *seg) : seg_(seg)
    {
    }
    WQE_CtrlSeg ctrl_seg() const
    {
        return WQE_CtrlSeg(seg_);
    }
    WQE_RaddrSeg raddr_seg() const
    {
        return WQE_RaddrSeg((char *) seg_ + sizeof(wqe_ctrl_seg));
    }
    WQE_DataSeg data_seg(size_t idx) const
    {
        return WQE_DataSeg((char *) seg_ + sizeof(wqe_ctrl_seg) +
                           sizeof(wqe_raddr_seg) + idx * sizeof(wqe_data_seg));
    }

private:
    void *seg_;
};

using wqe_atomic_seg = mlx5_wqe_atomic_seg;
using wqe_inl_data_seg = mlx5_wqe_inl_data_seg;

struct wqe_wait_en_seg
{
    uint8_t rsvd0[8];
    uint32_t pi;
    uint32_t obj_num;
};

class SendQueue
{
public:
    SendQueue(void *sq_start) : sq_start_(sq_start)
    {
    }
    WQE_Segments find_wqe(size_t idx)
    {
        auto *seg = get_send_wqe(idx);
        return WQE_Segments(seg);
    }
    void *get_send_wqe(size_t n)
    {
        auto ret = (uint64_t) sq_start_ + (n << MLX5_SEND_WQE_SHIFT);
        return (void *) ret;
    }

private:
    void *sq_start_;
};

class IQP
{
public:
    IQP(ibv_qp *qp)
    {
        struct mlx5dv_obj dv_obj = {};
        memset(&dv_obj, 0, sizeof(mlx5dv_obj));
        dv_obj.qp.in = qp;
        dv_obj.qp.out = &iqp_;
        int ret = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_QP);
        PLOG_IF(FATAL, ret != 0)
            << "Failed to retrieve internal qp: failed to mlx5dv_init_obj";
    }
    SendQueue sq()
    {
        return SendQueue(sq_start());
    }
    uint32_t read_send_db() const
    {
        auto val = *send_db();
        return ntohl(val);
    }
    void explain_snd_db() const
    {
        auto db = read_send_db();
        LOG(INFO) << "SND_DB: " << util::pre_hex(db) << ", "
                  << util::pre_bin(db);
    }
    volatile uint32_t *send_db()
    {
        return iqp_.dbrec + MLX5_SND_DBR;
    }
    volatile uint32_t *recv_db()
    {
        return iqp_.dbrec + MLX5_RCV_DBR;
    }

private:
    void *sq_start()
    {
        return iqp_.sq.buf;
    }
    void *rq_start()
    {
        return iqp_.rq.buf;
    }
    const volatile uint32_t *send_db() const
    {
        return iqp_.dbrec + MLX5_SND_DBR;
    }
    mlx5dv_qp iqp_;
};

class ICQ
{
public:
    ICQ(ibv_cq *cq)
    {
        struct mlx5dv_obj dv_obj = {};
        memset(&dv_obj, 0, sizeof(mlx5dv_obj));
        dv_obj.cq.in = cq;
        dv_obj.cq.out = &icq_;
        int ret = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_CQ);
        PLOG_IF(FATAL, ret != 0)
            << "Failed to retrieve internal cq: failed to mlx5dv_init_obj";
    }

private:
    mlx5dv_cq icq_;
};

}  // namespace mlx5