#pragma once
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>

#include "WRCtx.h"
#include "glog/logging.h"
#include "memory/manual_poison.h"
#include "util/CRTP.h"

class QP;

namespace rdma
{
template <typename T>
concept CqeFn = requires(T t, const ibv_exp_wc &wc)
{
    t(wc);
};

constexpr static inline auto empty_cqe_fn = [](const ibv_exp_wc &) -> void {};

/**
 * CQ is a wrapper to ibv_cq, which owns the resource
 * and manage the internal states.
 */
class CQ : public util::MakeShared<CQ>
{
public:
    constexpr static size_t kReport = false;
    CQ(ibv_context *context,
       int cqe,
       void *cq_context,
       ibv_comp_channel *channel,
       int comp_vector)
    {
        cq_ = CHECK_NOTNULL(
            ibv_create_cq(context, cqe, cq_context, channel, comp_vector));
    }
    CQ(const CQ &) = delete;
    CQ &operator=(CQ &) = delete;
    friend void swap(CQ &lhs, CQ &rhs) noexcept
    {
        using std::swap;
        swap(lhs.cq_, rhs.cq_);
        swap(lhs.ongoing_signaled_, rhs.ongoing_signaled_);
    }
    CQ(CQ &&rhs) noexcept
    {
        swap(*this, rhs);
    }
    CQ &operator=(CQ other)
    {
        swap(*this, other);
        return *this;
    }

    ~CQ()
    {
        if (cq_)
        {
            ibv_destroy_cq(cq_);
        }
    }
    ibv_cq *ibcq()
    {
        return cq_;
    }
    const ibv_cq *ibcq() const
    {
        return cq_;
    }
    size_t ongoing_signal() const
    {
        return ongoing_signaled_;
    }
    bool should_wait() const
    {
        return ongoing_signal() > 0;
    }

    template <typename Fn>
    int try_wait(size_t nr, Fn &&fn)
    {
        ibv_exp_wc wcs[32];
        int ret = ibv_exp_poll_cq(
            cq_, std::min(nr, (size_t) 32), wcs, sizeof(ibv_exp_wc));
        PLOG_IF(FATAL, ret < 0) << "** failed to ibv_exp_poll_cq";
        DCHECK_GE(ongoing_signaled_, ret) << "** internal error";
        ongoing_signaled_ -= ret;
        for (int i = 0; i < ret; ++i)
        {
            const auto &wc = wcs[i];
            LOG_IF(INFO, kReport)
                << "[CQ] polled get " << PRE(wc) << " " << (i + 1) << " / "
                << ret << " out of " << ongoing_signaled_;

            WRCtx *wrctx = (WRCtx *) wc.wr_id;
            const auto &wr = wrctx->wr();
            if (unlikely(wc.status != IBV_WC_SUCCESS))
            {
                LOG(FATAL) << "Operation failed: " << std::endl
                           << PRE(wc) << std::endl
                           << PRE(wr) << std::endl;
            }

            if constexpr (kEnablePoison)
            {
                // The unpoisoning of memory must be immediate
                auto *cur_wr = wrctx->head_;
                while (cur_wr)
                {
                    for (int sge_id = 0; sge_id < cur_wr->num_sge; ++sge_id)
                    {
                        const auto &sge = cur_wr->sg_list[sge_id];
                        // LOG(INFO) << "Unpoisoning for RDMA finished: "
                        //           << (void *) sge.addr << " " << sge.length;
                        memory::unpoison_memory_region((void *) sge.addr,
                                                       sge.length);
                    }
                    cur_wr = cur_wr->next;
                }
            }

            // then callback
            fn(wc);

            // finally postprocess
            auto *cur_wr = wrctx->head_;
            while (cur_wr)
            {
                // do user pp first
                auto *cur_wrctx = wr_to_ctx(cur_wr);
                if (cur_wrctx->user_pp_)
                {
                    cur_wrctx->user_pp_(cur_wrctx->user_data_);
                }
                // calculate the next before doing pp
                cur_wr = cur_wr->next;

                // ... before doing system's pp
                cur_wrctx->pp_((void *) cur_wrctx);
            }
        }
        return ret;
    }

    void wait(size_t nr)
    {
        DCHECK_LE(nr, ongoing_signaled_)
            << "** dead lock detected: waiting for more signaled wrs.";

        size_t remain = nr;
        while (remain)
        {
            int ok = try_wait(remain, empty_cqe_fn);
            DCHECK_LE(ok, remain);
            remain -= ok;
        }
    }
    void wait()
    {
        wait(ongoing_signaled_);
    }

    template <CqeFn Fn>
    void wait_fn(Fn &&fn)
    {
        wait(ongoing_signaled_, fn);
    }
    template <CqeFn Fn>
    void wait_fn(size_t nr, Fn &&fn)
    {
        wait(nr, fn);
    }

private:
    friend class QP;
    void post_signal(size_t nr)
    {
        ongoing_signaled_ += nr;
    }

    ibv_cq *cq_{};
    size_t ongoing_signaled_{};
};

}  // namespace rdma