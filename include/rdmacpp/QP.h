#pragma once
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>

#include <optional>

#include "./CQ.h"
#include "./QP_Debug.h"
#include "CoroContext.h"
#include "Rdma.h"
#include "glog/logging.h"
#include "memory/manual_poison.h"
#include "rdmacpp/Store.h"
#include "rdmacpp/WRCtx.h"
#include "util/Tracer.h"
#include "util/UP.h"

namespace rdma
{
/**
 * QP is a wrapper to ibv_qp, which owns the resource
 * and manage the internal states.
 * API:
 * // multiple times
 * qp->prepare_xxx(...)
 * // optional(ctx): coroutine
 * qp->commit(ctx)
 */
class QP : public util::MakeShared<QP>
{
public:
    constexpr static bool kReport = false;
    constexpr static bool kDebugHistory = false;
    using WRCtxT = rdma::WRCtx;
    QP(ibv_pd *pd,
       ibv_context *ctx,
       ibv_qp_type mode,
       CQ::Pointer cq,
       uint32_t max_send_wr,
       uint32_t max_recv_wr,
       uint32_t max_send_sge,
       uint32_t max_recv_sge,
       uint32_t max_inline,
       ibv_exp_res_domain *res_dom,
       uint32_t create_flags)
        : cq_(cq)
    {
        qp_ = CHECK_NOTNULL(createQueuePair(pd,
                                            ctx,
                                            mode,
                                            cq->ibcq(),
                                            cq->ibcq(),
                                            max_send_wr,
                                            max_recv_wr,
                                            max_send_sge,
                                            max_recv_sge,
                                            max_inline,
                                            res_dom,
                                            create_flags));
    }
    ~QP()
    {
        if (qp_)
        {
            CHECK(destroyQueuePair(qp_));
        }
    }
    QP(const QP &) = delete;
    QP &operator=(QP &) = delete;
    friend void swap(QP &lhs, QP &rhs) noexcept
    {
        using std::swap;
        swap(lhs.qp_, rhs.qp_);
        swap(lhs.cq_, rhs.cq_);
        swap(lhs.head_wr_, rhs.head_wr_);
        swap(lhs.tail_wr_, rhs.tail_wr_);
        swap(lhs.bad_wr_, rhs.bad_wr_);
    }
    QP(QP &&rhs) noexcept
    {
        swap(*this, rhs);
    }
    QP &operator=(QP other)
    {
        swap(*this, other);
        return *this;
    }

    auto *ibqp()
    {
        return qp_;
    }
    auto *ibcq()
    {
        return cq_->ibcq();
    }
    const auto *ibqp() const
    {
        return qp_;
    }
    CQ *cq() const
    {
        return cq_.get();
    }

    ibv_exp_send_wr *prepare_write(uint64_t dest,
                                   uint64_t source,
                                   size_t size,
                                   uint32_t lkey,
                                   uint32_t rkey)
    {
        auto *wrctx = alloc_wrctx();
        wrctx->pp_ = [](void *wrctx) { free_wrctx(wrctx); };

        auto &sge = wrctx->sge(0);
        auto &wr = wrctx->wr();
        fillSgeWr(sge, wr, source, size, lkey);
        wr.exp_opcode = IBV_EXP_WR_RDMA_WRITE;
        if (size <= 16 && !kEnablePoison)
        {
            wr.exp_send_flags |= IBV_EXP_SEND_INLINE;
        }
        wr.wr.rdma.remote_addr = dest;
        wr.wr.rdma.rkey = rkey;
        wr.wr_id = (uint64_t) wrctx;

        prepare_wr(&wr);
        wrctx->head_ = head_wr_;
        DCHECK_EQ(wr_to_ctx(&wr), wrctx);
        return &wr;
    }

    ibv_exp_send_wr *prepare_read(uint64_t source,
                                  uint64_t dest,
                                  size_t size,
                                  uint32_t lkey,
                                  uint32_t rkey)
    {
        auto *wrctx = alloc_wrctx();
        wrctx->pp_ = [](void *wrctx) { free_wrctx(wrctx); };

        auto &sge = wrctx->sge(0);
        auto &wr = wrctx->wr();
        fillSgeWr(sge, wr, source, size, lkey);
        wr.exp_opcode = IBV_EXP_WR_RDMA_READ;

        // NOTE: RDMA_READ must NOT have IBV_SEND_INLINE
        wr.wr.rdma.remote_addr = dest;
        wr.wr.rdma.rkey = rkey;
        wr.wr_id = (uint64_t) wrctx;

        prepare_wr(&wr);
        wrctx->head_ = head_wr_;
        DCHECK_EQ(wr_to_ctx(&wr), wrctx);
        return &wr;
    }

    ibv_exp_send_wr *prepare_faa(uint64_t dest,
                                 uint64_t source,
                                 size_t size,
                                 uint64_t add_val,
                                 uint64_t field_boundrary,
                                 uint32_t lkey,
                                 uint32_t rkey)
    {
        auto *wrctx = alloc_wrctx();
        wrctx->pp_ = [](void *wrctx) { free_wrctx(wrctx); };

        auto &sge = wrctx->sge(0);
        auto &wr = wrctx->wr();
        fillSgeWr(sge, wr, source, size, lkey);
        wr.exp_opcode = IBV_EXP_WR_EXT_MASKED_ATOMIC_FETCH_AND_ADD;
        wr.exp_send_flags = IBV_EXP_SEND_EXT_ATOMIC_INLINE;
        wr.wr_id = (uint64_t) wrctx;
        auto &ma = wr.ext_op.masked_atomics;
        if (size == 8)
        {
            ma.log_arg_sz = 3;
        }
        else if (size == 16)
        {
            ma.log_arg_sz = 4;
        }
        else if (size == 32)
        {
            ma.log_arg_sz = 5;
        }
        else
        {
            LOG(FATAL)
                << "Unsupported " << PRE(size)
                << ". If you believe it is instead supported, change me.";
        }
        ma.remote_addr = dest;
        ma.rkey = rkey;
        auto &op = ma.wr_data.inline_data.op.fetch_add;
        op.add_val = add_val;
        op.field_boundary = field_boundrary;

        DCHECK_EQ((uint64_t) dest % size, 0)
            << "** CAS addr should be size-aligned. got " << (void *) dest
            << ", expect aligment: " << size;

        prepare_wr(&wr);
        wrctx->head_ = head_wr_;
        DCHECK_EQ(wr_to_ctx(&wr), wrctx);
        return &wr;
    }

    ibv_exp_send_wr *prepare_cas(uint64_t dest,
                                 uint64_t source,
                                 size_t size,
                                 uint64_t compare,
                                 uint64_t compare_mask,
                                 uint64_t swap,
                                 uint64_t swap_mask,
                                 uint32_t lkey,
                                 uint32_t rkey)
    {
        auto *wrctx = alloc_wrctx();
        wrctx->pp_ = [](void *wrctx) { free_wrctx(wrctx); };

        auto &sge = wrctx->sge(0);
        auto &wr = wrctx->wr();
        fillSgeWr(sge, wr, source, size, lkey);
        wr.exp_opcode = IBV_EXP_WR_EXT_MASKED_ATOMIC_CMP_AND_SWP;
        wr.exp_send_flags = IBV_EXP_SEND_EXT_ATOMIC_INLINE;
        wr.wr_id = (uint64_t) wrctx;
        auto &ma = wr.ext_op.masked_atomics;
        if (size == 8)
        {
            ma.log_arg_sz = 3;
        }
        else if (size == 16)
        {
            ma.log_arg_sz = 4;
        }
        else if (size == 32)
        {
            ma.log_arg_sz = 5;
        }
        else
        {
            LOG(FATAL)
                << "Unsupported " << PRE(size)
                << ". If you believe it is instead supported, change me.";
        }
        ma.remote_addr = dest;
        ma.rkey = rkey;

        auto &op = ma.wr_data.inline_data.op.cmp_swap;
        op.compare_val = compare;
        op.swap_val = swap;
        op.compare_mask = compare_mask;
        op.swap_mask = swap_mask;

        DCHECK_EQ((uint64_t) dest % size, 0)
            << "** CAS addr should be size-byte aligned. got " << (void *) dest
            << ", expect alignment: " << size;

        prepare_wr(&wr);
        wrctx->head_ = head_wr_;
        DCHECK_EQ(wr_to_ctx(&wr), wrctx);

        return &wr;
    }

    // CONTRACT: mem_reg_list can dtor right after this call.
    // ibv_exp_mem_region
    // - .mr: the selected mr
    // - .base_addr: the start addr of a region in the mr
    // - .length: the length of the region
    void prepare_reg_list_umr(ibv_mr *umr,
                              ibv_exp_mem_region *mem_reg_list,
                              size_t num_mrs,
                              std::optional<uint64_t> base_addr)
    {
        auto *wrctx = alloc_wrctx();
        wrctx->pp_ = [](void *wrctx) { free_wrctx(wrctx); };

        size_t umr_len = 0;
        for (size_t i = 0; i < num_mrs; i++)
        {
            umr_len += mem_reg_list[i].length;
        }

        auto &wr = wrctx->wr();

        wr.ext_op.umr.umr_type = IBV_EXP_UMR_MR_LIST;
        wr.ext_op.umr.mem_list.mem_reg_list = mem_reg_list;

        wr.exp_send_flags = IBV_EXP_SEND_INLINE;

        wr.ext_op.umr.exp_access =
            IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_REMOTE_WRITE |
            IBV_EXP_ACCESS_REMOTE_READ | IBV_EXP_ACCESS_REMOTE_ATOMIC;
        wr.ext_op.umr.modified_mr = umr;
        if (base_addr)
        {
            wr.ext_op.umr.base_addr = *base_addr;
        }
        else
        {
            wr.ext_op.umr.base_addr = mem_reg_list[0].base_addr;
        }
        wr.ext_op.umr.num_mrs = num_mrs;
        wr.exp_opcode = IBV_EXP_WR_UMR_FILL;
        wr.wr_id = (uint64_t) wrctx;

        umr->length = umr_len;
        umr->addr = mem_reg_list[0].mr->addr;

        prepare_wr(&wr);
        wrctx->head_ = head_wr_;
        DCHECK_EQ(wr_to_ctx(&wr), wrctx);
    }

    struct RepeatedUMRCtx
    {
        ibv_exp_mem_repeat_block *mem_rep_list;
        size_t num_mrs;
        size_t *rpt_cnt;
    };
    // mem_rep_list
    // - .byte_count: array of length `ndim`
    // - .stride: array of length `ndim`
    void prepare_reg_repeated_umr(ibv_mr *umr,
                                  ibv_mr **mrs,
                                  size_t num_mrs,
                                  std::optional<uint64_t> base_addr,
                                  int rb_len,
                                  int rb_stride,
                                  int rb_count)
    {
        auto *wrctx = alloc_wrctx();
        auto *uctx = new RepeatedUMRCtx;
        wrctx->prv_ = uctx;
        wrctx->pp_ = [](void *wrctx)
        {
            WRCtxT *ctx = (WRCtxT *) wrctx;
            RepeatedUMRCtx *uctx = (RepeatedUMRCtx *) ctx->prv_;
            for (size_t i = 0; i < uctx->num_mrs; ++i)
            {
                delete[] uctx->mem_rep_list[i].byte_count;
                delete[] uctx->mem_rep_list[i].stride;
            }
            delete[] uctx->rpt_cnt;
            delete[] uctx->mem_rep_list;
            delete uctx;
            free_wrctx(wrctx);
        };

        int umr_len = 0;
        size_t ndim = 1;

        auto *mem_rep_list = new ibv_exp_mem_repeat_block[num_mrs]();
        uctx->mem_rep_list = mem_rep_list;
        uctx->num_mrs = num_mrs;

        for (size_t i = 0; i < num_mrs; i++)
        {
            mem_rep_list[i].byte_count = new size_t[ndim]();
            mem_rep_list[i].stride = new size_t[ndim]();
        }

        size_t *rpt_cnt = new size_t[ndim]();
        uctx->rpt_cnt = rpt_cnt;

        for (size_t i = 0; i < ndim; i++)
        {
            rpt_cnt[i] = rb_count;
        }

        for (size_t i = 0; i < num_mrs; i++)
        {
            mem_rep_list[i].base_addr = (uint64_t) (uintptr_t) mrs[i]->addr;
            mem_rep_list[i].byte_count[0] = rb_len;
            mem_rep_list[i].mr = mrs[i];
            mem_rep_list[i].stride[0] = rb_stride;

            umr_len += rb_count * mem_rep_list[i].byte_count[0];
        }

        auto &wr = wrctx->wr();
        wr.ext_op.umr.umr_type = IBV_EXP_UMR_REPEAT;
        wr.ext_op.umr.mem_list.rb.mem_repeat_block_list = mem_rep_list;
        wr.ext_op.umr.mem_list.rb.stride_dim = ndim;
        wr.ext_op.umr.mem_list.rb.repeat_count = rpt_cnt;

        wr.exp_send_flags = IBV_EXP_SEND_INLINE;

        wr.ext_op.umr.exp_access =
            IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_REMOTE_WRITE |
            IBV_EXP_ACCESS_REMOTE_READ | IBV_EXP_ACCESS_REMOTE_ATOMIC;
        wr.ext_op.umr.modified_mr = umr;
        if (base_addr)
        {
            wr.ext_op.umr.base_addr = *base_addr;
        }
        else
        {
            wr.ext_op.umr.base_addr = mem_rep_list[0].base_addr;
        }
        wr.ext_op.umr.num_mrs = num_mrs;
        wr.exp_send_flags |= IBV_EXP_SEND_SIGNALED;
        wr.exp_opcode = IBV_EXP_WR_UMR_FILL;
        wr.wr_id = (uint64_t) wrctx;

        umr->length = umr_len;
        umr->addr = mem_rep_list[0].mr->addr;

        prepare_wr(&wr);
        wrctx->head_ = head_wr_;
        DCHECK_EQ(wr_to_ctx(&wr), wrctx);
    }

    void prepare_wr(ibv_exp_send_wr *wr)
    {
        if (head_wr_ == nullptr)
        {
            DCHECK_EQ(tail_wr_, nullptr);
            head_wr_ = tail_wr_ = wr;
        }
        else
        {
            DCHECK_NE(tail_wr_, nullptr);
            tail_wr_->next = wr;
            tail_wr_ = wr;
        }
        while (unlikely(tail_wr_->next != nullptr))
        {
            tail_wr_ = tail_wr_->next;
        }
    }

    bool commit(CoroContext *ctx, util::TraceView trace = util::nulltrace)
    {
        return do_commit(true /* signal */, ctx, trace);
    }
    bool commit_no_wait(CoroContext *ctx,
                        util::TraceView trace = util::nulltrace)
    {
        return do_commit(false /* signal */, ctx, trace);
    }

    bool do_commit(bool signal, CoroContext *ctx, util::TraceView)
    {
        if constexpr (kDebugHistory)
        {
            auto *cur = head_wr_;
            while (cur)
            {
                history.current().add(Record{.wr = *cur});
                cur = cur->next;
            }
        }
        if (unlikely(!tail_wr_))
        {
            DCHECK_EQ(head_wr_, nullptr);
            return false;
        }

        // poisoning
        if constexpr (kEnablePoison)
        {
            auto *cur = head_wr_;
            while (cur)
            {
                for (int i = 0; i < cur->num_sge; ++i)
                {
                    const auto &sge = cur->sg_list[i];
                    // LOG(INFO) << "Poisoning " << (void *) sge.addr << " with
                    // "
                    //           << sge.length << " for ibv_post_send ";
                    memory::poison_memory_region((void *) sge.addr, sge.length);
                }
                cur = cur->next;
            }
        }

        if (signal)
        {
            tail_wr_->exp_send_flags |= IBV_EXP_SEND_SIGNALED;
        }

        auto *tail_wrctx = wr_to_ctx(tail_wr_);
        if (ctx)
        {
            tail_wrctx->ctx_ = ctx;
        }
        else
        {
            DCHECK_EQ(tail_wrctx->ctx_, nullptr);
        }

        LOG_IF(INFO, kReport) << PreWRLink(head_wr_);

        auto ret = ibv_exp_post_send(qp_, head_wr_, &bad_wr_);
        PLOG_IF(FATAL, ret != 0)
            << "** failed to ibv_exp_post_send: " << PRE(*bad_wr_);
        head_wr_ = tail_wr_ = bad_wr_ = nullptr;

        if (signal)
        {
            cq_->post_signal(1);
        }
        return true;
    }

private:
    ibv_qp *qp_{};
    CQ::Pointer cq_{};

    ibv_exp_send_wr *head_wr_{nullptr};
    ibv_exp_send_wr *tail_wr_{nullptr};
    ibv_exp_send_wr *bad_wr_{nullptr};

    // NOTE: use jemalloc. It has much better performance than new
    static WRCtxT *alloc_wrctx()
    {
        auto *addr = jemalloc(sizeof(WRCtxT));
        new (addr) WRCtxT();
        return (WRCtxT *) addr;
    }
    static void free_wrctx(void *p)
    {
        jefree(p);
    }
};

}  // namespace rdma