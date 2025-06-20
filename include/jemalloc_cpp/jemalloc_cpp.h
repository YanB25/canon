#pragma once
#include <jemalloc/jemalloc.h>

#include <cinttypes>

#include "./tag.h"
#include "Metrics.h"
#include "glog/logging.h"
#include "memory/allocator.h"
#include "memory/asan_interfaces.h"
#include "memory/manual_poison.h"
#include "util/CRTP.h"
#include "util/PreUtil.h"
#include "util/TLS.h"

namespace Jemalloc
{
template <typename T>
static T read(const char *name)
{
    T val;
    size_t size = sizeof(val);
    int ret = jemallctl(name, &val, &size, nullptr, 0);
    PLOG_IF(FATAL, ret) << "Failed to query " << util::pre(name) << ": "
                        << PRE(ret);
    return val;
}

template <typename T>
static void write(const char *name, const T &val)
{
    size_t size = sizeof(T);
    int ret = jemallctl(name, nullptr, 0, (void *) &val, size);
    PLOG_IF(FATAL, ret) << "Failed to write " << util::pre(name) << ": "
                        << PRE(ret);
}

template <typename R, typename W>
static R read_write(const char *name, const W &write_val)
{
    size_t w_size = sizeof(W);
    size_t r_size = sizeof(R);
    R read_val;
    int ret = jemallctl(name, &read_val, &r_size, (void *) &write_val, w_size);
    PLOG_IF(FATAL, ret) << "Failed to read_write " << util::pre(name) << ": "
                        << PRE(ret);
    return read_val;
}

inline void report()
{
    LOG(INFO) << PRE(Jemalloc::read<const char *>("opt.percpu_arena"));
    LOG(INFO) << PRE(Jemalloc::read<bool>("opt.background_thread"));
    LOG(INFO) << PRE(Jemalloc::read<bool>("opt.zero"));
    LOG(INFO) << PRE(Jemalloc::read<unsigned>("arenas.narenas"));
    LOG(INFO) << PRE(
        Jemalloc::read<uint64_t>("stats.background_thread.num_threads"));
}

template <enum Tag tag>
struct JemallocParam
{
    mem::BaseAllocator *alloc{};
};

template <enum Tag tag>
struct Arena
{
    constexpr static bool kReport = false;
    static void *extent_alloc_hook(extent_hooks_t *,
                                   void *new_addr,
                                   size_t size,
                                   size_t alignment,
                                   [[maybe_unused]] bool *zero,
                                   bool *,
                                   unsigned)
    {
        if (unlikely(new_addr != nullptr))
        {
            // according to doc, if new_addr is not nullptr
            // must return new_addr or nullptr
            return nullptr;
        }

        // NOTE: this allocator must be thread-safe
        // because the allocator is used "across" arenas.
        auto *alloc = Singleton<JemallocParam<tag>>().alloc;
        CHECK_NE(alloc, nullptr)
            << "** alloc is nullptr: did you call prepare_allocator(...)? tag: "
            << tag;

        char *ret = (char *) alloc->alloc(size, alignment);
        LOG_IF(INFO, kReport)
            << "[jemalloc] alloc " << size << " -> " << (void *) ret;
        if (unlikely(ret == nullptr))
        {
            return nullptr;
        }

        if (*zero)
        {
            memset(ret, 0, size);
        }

        DCHECK_EQ((uint64_t) ret % alignment, 0)
            << PRE((void *) ret, alignment, (uint64_t) ret % alignment);

        return ret;
    }

    static bool extent_dalloc_hook(
        extent_hooks_t *, void *, size_t, bool, unsigned)
    {
        return true;  // opt out
    }

    static void extent_destroy_hook(
        extent_hooks_t *, void *, size_t, bool, unsigned)
    {
        return;
    }

    static bool extent_commit_hook(
        extent_hooks_t *, void *addr, size_t size, size_t, size_t, unsigned)
    {
        memory::poison_memory_region(addr, size);
        return false;  // commit should always succeed
    }

    static bool extent_decommit_hook(
        extent_hooks_t *, void *addr, size_t size, size_t, size_t, unsigned)
    {
        memory::unpoison_memory_region(addr, size);
        return false;  // decommit should always succeed
    }

    static bool extent_purge_lazy_hook(
        extent_hooks_t *, void *, size_t, size_t, size_t, unsigned)
    {
        return true;  // opt out
    }

    static bool extent_purge_forced_hook(
        extent_hooks_t *, void *, size_t, size_t, size_t, unsigned)
    {
        return true;  // opt out
    }

    static bool extent_split_hook(
        extent_hooks_t *, void *, size_t, size_t, size_t, bool, unsigned)
    {
        return false;  // split should always succeed
    }

    static bool extent_merge_hook(
        extent_hooks_t *, void *, size_t, void *, size_t, bool, unsigned)
    {
        return false;  // merge should always succeed
    }
    constexpr const static extent_hooks_t hooks{extent_alloc_hook,
                                                extent_dalloc_hook,
                                                extent_destroy_hook,
                                                extent_commit_hook,
                                                extent_decommit_hook,
                                                extent_purge_lazy_hook,
                                                extent_purge_forced_hook,
                                                extent_split_hook,
                                                extent_merge_hook};
};

template <enum Tag tag, typename... Args>
void prepare_allocator(Args &&... args)
{
    auto &obj = Singleton<JemallocParam<tag>>();
    obj = JemallocParam<tag>{std::forward<Args>(args)...};
}

template <enum Tag tag>
class JemallocAllocator : public mem::BaseAllocator
{
public:
    using pointer = std::shared_ptr<JemallocAllocator>;
    static std::shared_ptr<JemallocAllocator> get_thread_allocator()
    {
        using AllocPtr = std::shared_ptr<JemallocAllocator<tag>>;
        auto &ret = TLS<AllocPtr>();
        if (unlikely(ret == nullptr))
        {
            ret = make_allocator();
        }
        return ret;
    }

    static std::shared_ptr<JemallocAllocator> make_allocator()
    {
        unsigned arena_id;
        size_t sz = sizeof(unsigned);

        const extent_hooks_t *new_hooks = &Arena<tag>::hooks;

        int ret = jemallctl("arenas.create",
                            &arena_id,
                            &sz,
                            (void *) &new_hooks,
                            sizeof(extent_hooks_t *));
        PLOG_IF(FATAL, ret) << "Failed to arenas.create: " << PRE(ret);

        unsigned cache_id = 0;
        ret = jemallctl("tcache.create", (void *) (&cache_id), &sz, nullptr, 0);
        PLOG_IF(FATAL, ret) << "Failed to tcache.create: " << PRE(ret);

        uint64_t id = MALLOCX_ARENA(arena_id) | MALLOCX_TCACHE(cache_id);
        // id |= MALLOCX_ALIGN(6);

        return std::make_shared<JemallocAllocator>(id);
    }

    JemallocAllocator(uint64_t id) : id_(id)
    {
    }
    void *alloc(size_t size, size_t align = 1) override
    {
        auto id = id_;
        if (align)
        {
            DCHECK(util::is_power_of_two(align));
            id |= MALLOCX_ALIGN(align);
        }
        auto *ret = jemallocx(size, id);
        if (likely(ret != nullptr))
        {
            u_.record_alloc(size);
            memory::unpoison_memory_region(ret, size);
        }
        else
        {
            // LOG(WARNING) << "[jemalloc] TAG " << tag << " failed to allocate
            // "
            //              << util::pre_byte(size);
        }
        return ret;
    }
    void free(void *addr) override
    {
        LOG(FATAL) << "TODO: without size, can not do poisoning";
        return jedallocx(addr, id_);
    }
    void free(void *addr, size_t size) override
    {
        u_.record_dealloc(size);
        memory::poison_memory_region(addr, size);
        return jedallocx(addr, id_);
    }
    auto usage() const
    {
        return u_;
    }

private:
    uint64_t id_{};
    AllocMetrics u_;
};

}  // namespace Jemalloc