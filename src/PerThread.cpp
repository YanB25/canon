#include "PerThread.h"

#include "CoroContext.h"

template <typename T>
T &PerCoro<T>::current(int cid)
{
    DCHECK_LT(cid, define::kMaxCoroNr);
    inner_.current()[cid];
}
template <typename T>
const T &PerCoro<T>::current(int cid) const
{
    DCHECK_LT(cid, define::kMaxCoroNr);
    inner_.current()[cid];
}

template <typename T>
T &PerCoro<T>::current(const CoroContext *ctx)
{
    DCHECK(ctx->is_worker()) << "** support master? Please double check";
    auto cid = DCHECK_NOTNULL(ctx)->coro_id();
    return current(cid);
}
template <typename T>
const T &PerCoro<T>::current(const CoroContext *ctx) const
{
    DCHECK(ctx->is_worker()) << "** support master? Please double check";
    auto cid = DCHECK_NOTNULL(ctx)->coro_id();
    return current(cid);
}