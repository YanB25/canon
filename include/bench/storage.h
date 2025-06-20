#pragma once
#include "glog/logging.h"
#include "storage_traits.h"
#include "util/Coro.h"

namespace bench
{
/**
 * Storage is a type defining any storage in experiments.
 * The *kind* of storage can be categorized into
 * - Persistent: whether or not remain across benchmark
 * - Locality: global, thread or coroutine.
 */
struct ID
{
    int node_id{-1};
    int thread_id{-1};
    int coro_id{kNotACoro};

    bool select_node(int n) const
    {
        CHECK_NE(node_id, -1);
        return node_id < n;
    }
    bool select_thread(int n) const
    {
        CHECK_NE(thread_id, -1);
        return thread_id < n;
    }
    bool select_worker_coro(int n) const
    {
        CHECK_NE(coro_id, kNotACoro);
        return coro_id != kMasterCoro && coro_id < n;
    }
    bool is_master_node() const
    {
        CHECK_NE(node_id, -1);
        return select_node(1);
    }
    bool is_master_thread() const
    {
        return select_thread(1);
    }
    bool is_master_coro() const
    {
        return coro_id == kMasterCoro;
    }
    bool is_worker_coro() const
    {
        return !is_master_coro() && coro_id != kNotACoro;
    }
    bool is_master_worker_coro() const
    {
        return select_worker_coro(1);
    }
};
std::ostream &operator<<(std::ostream &os, const ID &id)
{
    os << "{";
    if (id.node_id != -1)
    {
        os << "nid: " << id.node_id;
    }
    if (id.thread_id != -1)
    {
        os << " tid: " << id.thread_id;
    }
    if (id.is_master_coro())
    {
        os << ", master coro";
    }
    else if (id.coro_id != -1)
    {
        os << ", coro_id: " << id.coro_id;
    }
    os << "}";
    return os;
}
template <typename StorageSpec>
class Storage
{
public:
    using GLS = spec_gls_t<StorageSpec>;
    using TLS = spec_tls_t<StorageSpec>;
    using PTLS = spec_ptls_t<StorageSpec>;
    using CLS = spec_cls_t<StorageSpec>;
    using PCLS = spec_pcls_t<StorageSpec>;

    virtual const GLS &global() const
    {
        if (std::is_same_v<GLS, Void>)
        {
            LOG(FATAL) << "** Failed to access global: explicitly set to Void";
        }
        else
        {
            LOG(FATAL) << "** Failed to access global: lifecycle violated";
        }
    }
    virtual GLS &global()
    {
        const auto &ret = ((const Storage *) this)->global();
        return (GLS &) ret;
    }
    virtual const TLS &thread() const
    {
        if (std::is_same_v<TLS, Void>)
        {
            LOG(FATAL) << "** Failed to access thread: explicitly set to Void";
        }
        else
        {
            LOG(FATAL) << "** Failed to access thread: lifecycle violated";
        }
    }
    virtual TLS &thread()
    {
        const auto &ret = ((const Storage *) this)->thread();
        return (TLS &) ret;
    }
    virtual const PTLS &persistent_thread() const
    {
        if (std::is_same_v<PTLS, Void>)
        {
            LOG(FATAL) << "** Failed to access thread: explicitly set to Void";
        }
        else
        {
            LOG(FATAL) << "** Failed to access thread: lifecycle violated";
        }
    }
    virtual PTLS &persistent_thread()
    {
        const auto &ret = ((const Storage *) this)->persistent_thread();
        return (PTLS &) ret;
    }

    virtual const CLS &coroutine() const
    {
        if (std::is_same_v<CLS, Void>)
        {
            LOG(FATAL)
                << "** Failed to access coroutine: explicitly set to Void";
        }
        else
        {
            LOG(FATAL) << "** Failed to access coroutine: lifecycle violated";
        }
    }

    virtual CLS &coroutine()
    {
        const auto &ret = ((const Storage *) this)->coroutine();
        return (CLS &) ret;
    }
    virtual const PCLS &persistent_coroutine() const
    {
        if (std::is_same_v<PCLS, Void>)
        {
            LOG(FATAL)
                << "** Failed to access coroutine: explicitly set to Void";
        }
        else
        {
            LOG(FATAL) << "** Failed to access coroutine: lifecycle violated";
        }
    }

    virtual PCLS &persistent_coroutine()
    {
        const auto &ret = ((const Storage *) this)->persistent_coroutine();
        return (PCLS &) ret;
    }
    virtual ~Storage() = default;

    const ID &id() const
    {
        return id_;
    }
    ID &modified_id()
    {
        return id_;
    }

private:
    ID id_;
};

template <typename StorageSpec>
class GlobalStorage : public Storage<StorageSpec>
{
public:
    using Base = Storage<StorageSpec>;
    using GLS = typename Storage<StorageSpec>::GLS;
    GlobalStorage() = default;
    GlobalStorage(GLS *gls) : gls_(gls)
    {
    }
    const GLS &global() const override
    {
        if (gls_)
        {
            return *gls_;
        }
        return Base::global();
    }
    virtual ~GlobalStorage() = default;

private:
    GLS *gls_{nullptr};
};

template <typename StorageSpec>
class ThreadStorage : public GlobalStorage<StorageSpec>
{
public:
    using Base = GlobalStorage<StorageSpec>;
    using GLS = spec_gls_t<StorageSpec>;
    using TLS = spec_tls_t<StorageSpec>;
    using PTLS = spec_ptls_t<StorageSpec>;
    ThreadStorage(GLS *gls = nullptr, TLS *tls = nullptr, PTLS *ptls = nullptr)
        : Base(gls), tls_(tls), ptls_(ptls)
    {
    }

    const TLS &thread() const override
    {
        if (tls_)
        {
            return *tls_;
        }
        return Base::thread();
    }
    const PTLS &persistent_thread() const override
    {
        if (ptls_)
        {
            return *ptls_;
        }
        return Base::persistent_thread();
    }
    virtual ~ThreadStorage() = default;

private:
    TLS *tls_{nullptr};
    PTLS *ptls_{nullptr};
};

template <typename StorageSpec>
class CoroStorage : public ThreadStorage<StorageSpec>
{
public:
    using Base = ThreadStorage<StorageSpec>;
    using GLS = spec_gls_t<StorageSpec>;
    using TLS = spec_tls_t<StorageSpec>;
    using PTLS = spec_ptls_t<StorageSpec>;
    using CLS = spec_cls_t<StorageSpec>;
    using PCLS = spec_pcls_t<StorageSpec>;
    CoroStorage(GLS *gls = nullptr,
                TLS *tls = nullptr,
                PTLS *ptls = nullptr,
                CLS *cls = nullptr,
                PCLS *pcls = nullptr)
        : Base(gls, tls, ptls), cls_(cls), pcls_(pcls)
    {
    }

    const CLS &coroutine() const override
    {
        if (cls_)
        {
            return *cls_;
        }
        return Base::coroutine();
    }
    const PCLS &persistent_coroutine() const override
    {
        if (pcls_)
        {
            return *pcls_;
        }
        return Base::persistent_coroutine();
    }
    virtual ~CoroStorage() = default;

private:
    CLS *cls_{nullptr};
    PCLS *pcls_{nullptr};
};

}  // namespace bench