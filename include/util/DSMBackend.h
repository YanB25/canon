#pragma once
#include "CacheBackend.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "util/Page.h"

namespace util
{
using DSMBackendKey = GlobalAddress;
using DSMBackendValue = util::Page;
class DSMBackend : public ICacheBackend<DSMBackendKey, DSMBackendValue>
{
public:
    using Base = ICacheBackend<DSMBackendKey, DSMBackendValue>;
    using Entry = typename Base::Entry;
    using K = DSMBackendKey;
    using V = DSMBackendValue;
    DSMBackend(DSM::pointer dsm) : dsm_(dsm)
    {
    }
    static std::shared_ptr<DSMBackend> make_ptr(DSM::pointer dsm)
    {
        return std::make_shared<DSMBackend>(dsm);
    }
    // CONTRACT: `value` lives until commit()
    void put_async(const K &key, const V &value, CoroContext *ctx) override
    {
        evict_nr_.current()++;

        GlobalAddress gaddr = key;

        DCHECK_EQ(value.size(), kInternalPageSize);
        DCHECK(value.is_DMA());
        dsm_->prepare_write(value.data(), gaddr, kInternalPageSize, ctx);
    }
    V get_async(const K &key, CoroContext *ctx) override
    {
        fetch_nr_.current()++;

        GlobalAddress gaddr = key;

        V ret = dsm_->get_rdma_page(kInternalPageSize);
        DCHECK(ret.is_DMA());
        dsm_->prepare_read(ret.data(), gaddr, kInternalPageSize, ctx);
        return ret;
    }

    // wait until all the async command has finished
    void sync(CoroContext *ctx) override
    {
        dsm_->commit(ctx);
    }
    size_t evict_nr() const
    {
        auto f = [](const size_t &acc, const size_t &cur) -> size_t
        { return acc + cur; };
        return evict_nr_.accumulate(f, 0ull);
    }
    size_t fetch_nr() const
    {
        auto f = [](const size_t &acc, const size_t &cur) -> size_t
        { return acc + cur; };
        return fetch_nr_.accumulate(f, 0ull);
    }

private:
    DSM::pointer dsm_;
    Perthread<size_t> evict_nr_;
    Perthread<size_t> fetch_nr_;
};
}  // namespace util