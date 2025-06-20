#pragma once
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>

#include <iostream>
#include <optional>

#include "CoroContext.h"
#include "Pool.h"
#include "jemalloc/jemalloc.h"
#include "util/TLS.h"

namespace rdma
{
using post_process_t = void (*)(void *wrctx);

// TODO: don't use macro
#define kNullWr ((ibv_exp_send_wr *) 0xaabbccdd11223344)

// one WRCtx for one WR
struct WRCtx
{
    WRCtx(size_t sge_nr = 1) : sge_nr_(sge_nr)
    {
        CHECK_EQ(sge_nr, 1);
        // sges_ = new ibv_sge[sge_nr];
        // sges_ = (ibv_sge *) jemalloc(sge_nr * sizeof(ibv_sge));
    }
    ~WRCtx()
    {
        // delete[] sges_;
        // jefree(sges_);
    }
    WRCtx &operator=(const WRCtx &) = delete;
    WRCtx(const WRCtx &) = delete;
    WRCtx &operator=(WRCtx &&) = default;
    WRCtx(WRCtx &&) = default;

    ibv_exp_send_wr wr_{};
    ibv_exp_send_wr *head_{};
    // ibv_sge *sges_{};
    ibv_sge sges_[1]{};
    size_t sge_nr_{};
    void *prv_{};
    post_process_t pp_{};
    CoroContext *ctx_{};

    post_process_t user_pp_{};
    void *user_data_{};

    void *user_data()
    {
        return user_data_;
    }

    ibv_exp_send_wr &wr()
    {
        return wr_;
    }
    const ibv_exp_send_wr &wr() const
    {
        return wr_;
    }
    ibv_sge *sges()
    {
        return sges_;
    }
    ibv_sge &sge(size_t idx)
    {
        DCHECK_LT(idx, sge_nr());
        return sges_[idx];
    }

    size_t sge_nr() const
    {
        return sge_nr_;
    }
};

inline WRCtx *wr_to_ctx(const ibv_exp_send_wr *wr)
{
    uint64_t addr = (uint64_t) wr;
    WRCtx *ret = (WRCtx *) (addr - offsetof(WRCtx, wr_));
    return ret;
}

inline std::ostream &operator<<(std::ostream &os, const WRCtx &ctx)
{
    os << "{WRCtx wr: [";
    // const ibv_exp_send_wr *wr = &ctx.wrs_[0];
    auto *wr = &(ctx.wr());
    while (wr != nullptr && wr != kNullWr)
    {
        os << *wr << " ";
        wr = wr->next;
    }
    os << "] ctx: " << (void *) ctx.ctx_ << "}";
    return os;
}

}  // namespace rdma