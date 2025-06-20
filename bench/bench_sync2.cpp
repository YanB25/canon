#include <infiniband/verbs_exp.h>
#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "Common.h"
#include "DSMCache.h"
#include "WRLock.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
#include "util/ZipRand.h"
#include "util/gflags_def.h"
#include "util/stacktrace.h"
#include "util/thread_id.h"

DEFINE_double(z, 0.99, "The skewness.");
DEFINE_uint64(data_nr, 1024, "Number of data.");
DEFINE_bool(cow_use_cache,
            true,
            "Whether or not COW synchronization cache the location of the "
            "object (reduce 1 RDMA RTT)");

struct ThreadContext
{
    GlobalAddress gaddr;
};
struct Spec
{
    using PTLS = ThreadContext;
    struct TLS
    {
        size_t op{};
    };
    struct CLS
    {
        std::array<GlobalAddress, 64> gaddrs;
        size_t cur_idx{0};
    };
};

struct PrvCoroCtx
{
    Buffer lock_buf;
    Buffer unlock_buf;
    Buffer write_buf;
    Buffer read_buf;
    Buffer cas_buf;
};

// [Object][CRC][lock]
// RDMA_write from lower to higher, so put CRC after T
// so put lock at last, so can unlock directly.
struct CRCLayout
{
    using lock_t = uint64_t;
    using crc_t = uint64_t;
    size_t obj_size;
    static size_t object_offset()
    {
        return 0;
    }
    size_t object_size() const
    {
        return obj_size;
    }
    size_t crc_offset() const
    {
        return object_size();
    }
    static size_t crc_size()
    {
        return sizeof(crc_t);
    }
    size_t lock_offset() const
    {
        return crc_offset() + crc_size();
    }
    static size_t lock_size()
    {
        return sizeof(lock_t);
    }
    size_t total_size() const
    {
        return total_size(object_size());
    }
    static size_t total_size(size_t obj_size)
    {
        return obj_size + crc_size() + lock_size();
    }
};

// for local
class CRCObj
{
public:
    using Layout = CRCLayout;
    CRCObj(char *buf, size_t obj_size) : buf_(buf), obj_size_(obj_size)
    {
    }
    // make it consistent
    void init()
    {
        auto l = layout();
        char *obj_buf = buf_ + l.object_offset();
        auto *crc_buf = pcrc();
        auto *lock_buf = plock();
        memset(obj_buf, 0, l.object_size());
        *crc_buf = CityHash64(obj_buf, l.object_size());
        *lock_buf = 0;
    }
    CRCLayout layout() const
    {
        return CRCLayout{obj_size_};
    }
    bool is_locked() const
    {
        return !is_unlocked();
    }
    bool is_unlocked() const
    {
        return *plock() == 0;
    }
    char *pobject()
    {
        return buf_;
    }
    Layout::lock_t *plock() const
    {
        return (Layout::lock_t *) (buf_ + layout().lock_offset());
    }
    Layout::crc_t *pcrc() const
    {
        return (Layout::crc_t *) (buf_ + layout().crc_offset());
    }

private:
    char *buf_;
    size_t obj_size_;
};

// [Object][Version][lock]
// RDMA_write from lower to higher, so put CRC after T
// so put lock at last, so can unlock directly.
struct RFRLayout
{
    using lock_t = uint64_t;
    using version_t = uint64_t;
    size_t obj_size;
    static size_t object_offset()
    {
        return 0;
    }
    size_t object_size() const
    {
        return obj_size;
    }
    size_t version_offset() const
    {
        return object_size();
    }
    static size_t version_size()
    {
        return sizeof(version_t);
    }
    size_t lock_offset() const
    {
        return version_offset() + version_size();
    }
    static size_t lock_size()
    {
        return sizeof(lock_t);
    }
    size_t total_size() const
    {
        return total_size(object_size());
    }
    static size_t total_size(size_t obj_size)
    {
        return obj_size + version_size() + lock_size();
    }
};

// for local
class RFRObj
{
public:
    using Layout = RFRLayout;
    RFRObj(char *buf, size_t obj_size) : buf_(buf), obj_size_(obj_size)
    {
    }
    // make it consistent
    void init()
    {
        auto l = layout();
        char *obj_buf = buf_ + l.object_offset();
        auto *version_buf = pversion();
        auto *lock_buf = plock();
        memset(obj_buf, 0, l.object_size());
        *version_buf = 0;
        *lock_buf = 0;
    }
    RFRLayout layout() const
    {
        return RFRLayout{obj_size_};
    }
    bool is_locked() const
    {
        return !is_unlocked();
    }
    bool is_unlocked() const
    {
        return *plock() == 0;
    }
    char *pobject()
    {
        return buf_;
    }
    Layout::lock_t *plock() const
    {
        return (Layout::lock_t *) (buf_ + layout().lock_offset());
    }
    Layout::version_t *pversion() const
    {
        return (Layout::version_t *) (buf_ + layout().version_offset());
    }

private:
    char *buf_;
    size_t obj_size_;
};

// for remote
class CRC
{
public:
    using Layout = CRCLayout;
    CRC(DSM *dsm, CoroContext *ctx, GlobalAddress obj_gaddr, size_t obj_size)
        : dsm_(dsm), ctx_(ctx), obj_gaddr_(obj_gaddr), obj_size_(obj_size)
    {
        rdma_buf_ = dsm_->get_rdma_buffer(total_size());
    }
    ~CRC()
    {
        dsm_->put_rdma_buffer(std::move(rdma_buf_));
    }
    bool try_lock()
    {
        auto lock_gaddr = get_lock_gaddr();
        uint64_t *lock_rdma_buf = get_lock_rdma_buf();
        dsm_->prepare_cas(lock_gaddr,
                          layout().lock_size(),
                          0,
                          0xffffffffffffffff,
                          1,
                          0xffffffffffffffff,
                          lock_rdma_buf,
                          false,
                          ctx_);
        dsm_->commit(ctx_);
        uint64_t got_lock_val = *lock_rdma_buf;
        if (got_lock_val == 0)
        {
            return true;
        }
        else
        {
            CHECK_EQ(got_lock_val, 1);
            return false;
        }
    }
    size_t lock()
    {
        size_t try_nr = 0;
        while (true)
        {
            bool ok = try_lock();
            try_nr++;
            if (ok)
            {
                return try_nr;
            }
        }
    }
    GlobalAddress get_lock_gaddr()
    {
        return obj_gaddr_ + layout().lock_offset();
    }
    uint64_t *get_lock_rdma_buf()
    {
        return (uint64_t *) (rdma_buf_.buffer + layout().lock_offset());
    }
    uint64_t *get_crc_rdma_buf()
    {
        return (uint64_t *) (rdma_buf_.buffer + layout().crc_offset());
    }
    char *get_object_rdma_buf()
    {
        return rdma_buf_.buffer + layout().object_offset();
    }
    void prepare_write()
    {
        CRCObj obj(rdma_buf_.buffer, obj_size_);
        *obj.plock() = 0;
        *obj.pcrc() = CityHash64(obj.pobject(), obj_size_);
        DCHECK(obj.is_unlocked()) << "** inconsistent";
    }
    void write_unlock()
    {
        // assuming RDMA write order:
        // write object first, then CRC, then lock.
        prepare_write();
        dsm_->prepare_write(
            (char *) rdma_buf_.buffer, obj_gaddr_, total_size(), false, ctx_);
        if constexpr (debug())
        {
            CRCObj crc(rdma_buf_.buffer, obj_size_);
            CHECK(crc.is_unlocked())
                << "** inconsistent: " << PRE(*crc.plock());
        }
        dsm_->commit(ctx_);
    }
    // is consistent, read value
    std::pair<bool, void *> try_read()
    {
        dsm_->prepare_read(
            rdma_buf_.buffer, obj_gaddr_, total_size(), false, ctx_);
        dsm_->commit(ctx_);
        uint64_t got_crc = *get_crc_rdma_buf();
        auto *object_buf = get_object_rdma_buf();
        uint64_t expect_crc = CityHash64(object_buf, layout().object_size());
        if (got_crc == expect_crc)
        {
            // LOG(INFO) << "debug: crc read ok: " << (void *) got_crc << " vs "
            //           << (void *) expect_crc;
            return {true, object_buf};
        }
        else
        {
            // LOG(INFO) << "debug: crc read not ok: " << (void *) got_crc
            //           << " vs " << (void *) expect_crc;
            return {false, object_buf};
        }
    }
    std::pair<size_t, void *> read()
    {
        size_t try_nr = 0;
        while (true)
        {
            auto [ok, buf] = try_read();
            try_nr++;
            if (ok)
            {
                return {try_nr, buf};
            }
        }
    }
    Layout layout() const
    {
        return Layout{obj_size_};
    }
    size_t total_size() const
    {
        return layout().total_size();
    }

private:
    DSM *dsm_;
    CoroContext *ctx_;
    GlobalAddress obj_gaddr_;
    size_t obj_size_;

    Buffer rdma_buf_;
};

class RFR
{
public:
    using Layout = RFRLayout;
    RFR(DSM *dsm, CoroContext *ctx, GlobalAddress obj_gaddr, size_t obj_size)
        : dsm_(dsm), ctx_(ctx), obj_gaddr_(obj_gaddr), obj_size_(obj_size)
    {
        rdma_buf_ = dsm_->get_rdma_buffer(total_size());
        read_bufs_.emplace_back(dsm_->get_rdma_buffer(layout().version_size()));
        read_bufs_.emplace_back(dsm_->get_rdma_buffer(layout().object_size()));
        read_bufs_.emplace_back(dsm_->get_rdma_buffer(layout().version_size()));
    }
    ~RFR()
    {
        dsm_->put_rdma_buffer(std::move(rdma_buf_));
        for (auto &&buf : read_bufs_)
        {
            dsm_->put_rdma_buffer(std::move(buf));
        }
    }
    bool try_lock()
    {
        auto lock_gaddr = get_lock_gaddr();
        uint64_t *lock_rdma_buf = get_lock_rdma_buf();
        dsm_->prepare_cas(lock_gaddr,
                          layout().lock_size(),
                          0,
                          0xffffffffffffffff,
                          1,
                          0xffffffffffffffff,
                          lock_rdma_buf,
                          false,
                          ctx_);
        dsm_->commit(ctx_);
        uint64_t got_lock_val = *lock_rdma_buf;
        if (got_lock_val == 0)
        {
            return true;
        }
        else
        {
            CHECK_EQ(got_lock_val, 1);
            return false;
        }
    }
    size_t lock()
    {
        size_t try_nr = 0;
        while (true)
        {
            bool ok = try_lock();
            try_nr++;
            if (ok)
            {
                return try_nr;
            }
        }
    }
    GlobalAddress get_lock_gaddr()
    {
        return obj_gaddr_ + layout().lock_offset();
    }
    uint64_t *get_lock_rdma_buf()
    {
        return (uint64_t *) (rdma_buf_.buffer + layout().lock_offset());
    }
    char *get_object_rdma_buf()
    {
        return rdma_buf_.buffer + layout().object_offset();
    }
    void prepare_write()
    {
        CRCObj obj(rdma_buf_.buffer, obj_size_);
        *obj.plock() = 0;
        *obj.pcrc() = CityHash64(obj.pobject(), obj_size_);
        DCHECK(obj.is_unlocked()) << "** inconsistent";
    }
    void write_unlock()
    {
        // assuming RDMA write order:
        // write object first, then CRC, then lock.
        prepare_write();
        dsm_->prepare_write(
            (char *) rdma_buf_.buffer, obj_gaddr_, total_size(), false, ctx_);
        if constexpr (debug())
        {
            CRCObj crc(rdma_buf_.buffer, obj_size_);
            CHECK(crc.is_unlocked())
                << "** inconsistent: " << PRE(*crc.plock());
        }
        dsm_->commit(ctx_);
    }
    // is consistent, read value
    std::pair<bool, void *> try_read()
    {
        auto version_gaddr = obj_gaddr_ + layout().version_offset();
        auto version_size = layout().version_size();
        dsm_->prepare_read(
            read_bufs_[0].buffer, version_gaddr, version_size, false, ctx_);
        auto obj_gaddr = obj_gaddr_ + layout().object_offset();
        auto *wrd = dsm_->prepare_read(
            read_bufs_[1].buffer, obj_gaddr, obj_size_, false, ctx_);
        wrd->exp_send_flags |= IBV_EXP_SEND_FENCE;
        auto *wr3 = dsm_->prepare_read(
            read_bufs_[2].buffer, version_gaddr, version_size, false, ctx_);
        wr3->exp_send_flags |= IBV_EXP_SEND_FENCE;
        dsm_->commit(ctx_);
        return {true, read_bufs_[1].buffer};
        // dsm_->prepare_read(
        //     rdma_buf_.buffer, obj_gaddr_, total_size(), false, ctx_);
        // dsm_->commit(ctx_);
        // uint64_t got_crc = *get_crc_rdma_buf();
        // auto *object_buf = get_object_rdma_buf();
        // uint64_t expect_crc = CityHash64(object_buf, layout().object_size());
        // if (got_crc == expect_crc)
        // {
        //     // LOG(INFO) << "debug: crc read ok: " << (void *) got_crc << "
        //     vs "
        //     //           << (void *) expect_crc;
        //     return {true, object_buf};
        // }
        // else
        // {
        //     // LOG(INFO) << "debug: crc read not ok: " << (void *) got_crc
        //     //           << " vs " << (void *) expect_crc;
        //     return {false, object_buf};
        // }
    }
    std::pair<size_t, void *> read()
    {
        size_t try_nr = 0;
        while (true)
        {
            auto [ok, buf] = try_read();
            try_nr++;
            if (ok)
            {
                return {try_nr, buf};
            }
        }
    }
    size_t total_size() const
    {
        // return layout().total_size();
        return obj_size_ + 8;
    }

    Layout layout() const
    {
        return Layout{obj_size_};
    }

private:
    DSM *dsm_;
    CoroContext *ctx_;
    GlobalAddress obj_gaddr_;
    size_t obj_size_;

    Buffer rdma_buf_;
    std::vector<Buffer> read_bufs_;
};

struct COWLayout
{
    using ptr_t = uint64_t;
    static size_t ptr_size()
    {
        return sizeof(uint64_t);
    }
};

class COWObj
{
public:
    COWObj(char *buf) : buf_(buf)
    {
    }
    void init()
    {
        auto *ptr_buf = (COWLayout::ptr_t *) buf_;
        *ptr_buf = 0;
    }
    COWLayout::ptr_t *ptr_buf()
    {
        return (COWLayout::ptr_t *) buf_;
    }

private:
    char *buf_;
};

// [ptr] => [object]
class COW
{
public:
    using Layout = COWLayout;
    COW(DSM *dsm, CoroContext *ctx, GlobalAddress ptr_gaddr, size_t obj_size)
        : dsm_(dsm), ctx_(ctx), ptr_gaddr_(ptr_gaddr), obj_size_(obj_size)
    {
        obj_buf_ = dsm_->get_rdma_buffer(object_size());
        ptr_buf_ = dsm_->get_rdma_buffer(ptr_size());
    }
    ~COW()
    {
        dsm_->put_rdma_buffer(std::move(obj_buf_));
        dsm_->put_rdma_buffer(std::move(ptr_buf_));
    }

    [[nodiscard]] char *prepare_write()
    {
        if (new_obj_gaddr_.is_null())
        {
            new_obj_gaddr_ = dsm_->alloc(object_size());
            if (unlikely(new_obj_gaddr_.is_null()))
            {
                new_obj_gaddr_ = dsm_->alloc(object_size());
            }
            CHECK(!new_obj_gaddr_.is_null()) << "** possible run out of memory";
            dsm_->prepare_write(
                obj_buf_.buffer, new_obj_gaddr_, object_size(), false, ctx_);
        }
        return (char *) obj_buf_.buffer;
    }

    bool try_write()
    {
        // try to CAS
        CHECK(!new_obj_gaddr_.is_null());
        uint64_t *ptr_buf = (uint64_t *) get_ptr_rdma_buffer();
        uint64_t expect_old = *ptr_buf;
        dsm_->prepare_cas(ptr_gaddr_,
                          ptr_size(),
                          *ptr_buf,
                          0xffffffffffffffff,
                          new_obj_gaddr_.val,
                          0xffffffffffffffff,
                          ptr_buf,
                          false,
                          ctx_);
        dsm_->commit(ctx_);

        if (*ptr_buf == expect_old)
        {
            // CAS ok
            // free the old
            *ptr_buf = new_obj_gaddr_.val;
            new_obj_gaddr_ = GlobalAddress::Null();
            dsm_->free(GlobalAddress((void *) expect_old), object_size());
            return true;
        }
        else
        {
            // CAS failed
            return false;
        }
    }
    size_t write()
    {
        size_t try_nr = 0;
        while (true)
        {
            bool ok = try_write();
            try_nr++;
            if (ok)
            {
                return try_nr;
            }
        }
    }
    char *read()
    {
        dsm_->prepare_read(
            ptr_buf_.buffer, ptr_gaddr_, ptr_size(), false, ctx_);
        dsm_->commit(ctx_);
        uint64_t got_gaddr = *(uint64_t *) ptr_buf_.buffer;
        GlobalAddress obj_gaddr = GlobalAddress((void *) got_gaddr);

        dsm_->prepare_read(
            obj_buf_.buffer, obj_gaddr, object_size(), false, ctx_);
        dsm_->commit(ctx_);
        return obj_buf_.buffer;
    }
    char *read_direct(Layout::ptr_t cached_obj_addr)
    {
        GlobalAddress obj_gaddr((void *) cached_obj_addr);
        dsm_->prepare_read(
            obj_buf_.buffer, obj_gaddr, object_size(), false, ctx_);
        dsm_->commit(ctx_);
        return obj_buf_.buffer;
    }

    // user may also call this to modify the object inplace
    char *get_object_rdma_buffer() const
    {
        return obj_buf_.buffer;
    }
    char *get_ptr_rdma_buffer() const
    {
        return ptr_buf_.buffer;
    }
    static COWLayout layout()
    {
        return COWLayout{};
    }
    size_t object_size() const
    {
        return obj_size_;
    }
    static size_t ptr_size()
    {
        return layout().ptr_size();
    }
    Layout::ptr_t *ptr_buf()
    {
        return (Layout::ptr_t *) ptr_buf_.buffer;
    }

private:
    DSM *dsm_;
    CoroContext *ctx_;
    GlobalAddress ptr_gaddr_;
    GlobalAddress obj_gaddr_{};
    GlobalAddress new_obj_gaddr_{};
    size_t obj_size_;

    Buffer obj_buf_;
    Buffer ptr_buf_;
};

struct CachelineLayout
{
    using cl_version_t = uint8_t;
    using g_version_t = uint32_t;
    using lock_t = uint32_t;
    using g_version_lock_t = uint64_t;
    size_t obj_size_;
    size_t object_size() const
    {
        return obj_size_;
    }
    size_t object_embeded_size() const
    {
        return object_embeded_size(object_size());
    }
    static size_t object_embeded_size(size_t obj_size)
    {
        return trunk_nr(obj_size) * cacheline_size();
    }
    constexpr static size_t object_offset()
    {
        return 0;
    }
    constexpr static size_t cacheline_size()
    {
        return 64;
    }
    constexpr static size_t cl_version_size()
    {
        return sizeof(cl_version_t);
    }
    size_t g_version_offset() const
    {
        return lock_offset() + lock_size();
    }
    constexpr static size_t g_version_size()
    {
        return sizeof(g_version_t);
    }
    size_t lock_offset() const
    {
        return object_embeded_size();
    }
    constexpr size_t lock_version_size()
    {
        return g_version_size() + lock_size();
    }
    static size_t total_size(size_t obj_size)
    {
        return object_embeded_size(obj_size) + g_version_size() + lock_size();
    }
    size_t total_size() const
    {
        return object_embeded_size() + g_version_size() + lock_size();
    }
    constexpr static size_t lock_size()
    {
        return sizeof(lock_t);
    }
    constexpr static size_t cachedata_size()
    {
        return cacheline_size() - cl_version_size();
    }
    static size_t trunk_nr(size_t obj_size)
    {
        return round_up_div(obj_size, cachedata_size());
    }
    size_t trunk_nr() const
    {
        return trunk_nr(object_size());
    }

    void obj_to_buf(char *to_buf, const char *from_buf)
    {
        auto aligned_trunk_nr = obj_size_ / cachedata_size();
        for (size_t i = 0; i < aligned_trunk_nr; ++i)
        {
            memcpy(to_buf + i * cachedata_size(),
                   from_buf + i * cacheline_size() + cl_version_size(),
                   cachedata_size());
        }
        auto remain = obj_size_ % cachedata_size();
        if (remain)
        {
            auto trunk_id = trunk_nr() - 1;
            memcpy(to_buf + trunk_id * cachedata_size(),
                   from_buf + trunk_id * cacheline_size() + cl_version_size(),
                   remain);
        }
    }

    void obj_fill(char *buf, int fill, g_version_t gv, lock_t lock_val)
    {
        auto aligned_trunk_nr = obj_size_ / cachedata_size();
        cl_version_t cl_v = (cl_version_t) gv;  // truncate

        // for those cachedata blocks
        for (size_t i = 0; i < aligned_trunk_nr; ++i)
        {
            // set cacheline version
            auto *p_cl_v = (cl_version_t *) (buf + i * cacheline_size());
            *p_cl_v = cl_v;
            // set cacheline data
            char *src_buf = buf + i * cacheline_size() + cl_version_size();
            memset(src_buf, fill, cachedata_size());
        }
        // for the last remaining cache block
        auto remain = obj_size_ % cachedata_size();
        if (remain)
        {
            auto trunk_id = trunk_nr() - 1;
            // set cacheline version
            auto *p_cl_v = (cl_version_t *) (buf + trunk_id * cacheline_size());
            *p_cl_v = cl_v;
            // set cacheline data
            char *src_buf =
                buf + trunk_id * cacheline_size() + cl_version_size();
            memset(src_buf, fill, remain);
        }

        // set version & lock
        auto *p_g_version = (g_version_t *) (buf + g_version_offset());
        *p_g_version = gv;
        auto *p_lock = (lock_t *) (buf + lock_offset());
        *p_lock = lock_val;
    }

    void obj_from_buf(char *buf,
                      const char *from_buf,
                      g_version_t gv,
                      lock_t lock_val)
    {
        auto aligned_trunk_nr = obj_size_ / cachedata_size();
        cl_version_t cl_v = (cl_version_t) gv;  // truncate

        // for those cachedata blocks
        for (size_t i = 0; i < aligned_trunk_nr; ++i)
        {
            // set cacheline version
            auto *p_cl_v = (cl_version_t *) (buf + i * cacheline_size());
            *p_cl_v = cl_v;
            // set cacheline data
            char *src_buf = buf + i * cacheline_size() + cl_version_size();
            const char *dest_buf = from_buf + i * cachedata_size();
            memcpy(src_buf, dest_buf, cachedata_size());
        }
        // for the last remaining cache block

        auto remain = obj_size_ % cachedata_size();
        if (remain)
        {
            auto trunk_id = trunk_nr() - 1;
            // set cacheline version
            auto *p_cl_v = (cl_version_t *) (buf + trunk_id * cacheline_size());
            *p_cl_v = cl_v;
            // set cacheline data
            char *src_buf =
                buf + trunk_id * cacheline_size() + cl_version_size();
            const char *dest_buf = from_buf + trunk_id * cachedata_size();
            memcpy(src_buf, dest_buf, remain);
        }

        // set version & lock
        auto *p_g_version = (g_version_t *) (buf + g_version_offset());
        *p_g_version = gv;
        auto *p_lock = (lock_t *) (buf + lock_offset());
        *p_lock = lock_val;
    }
};

class CachelineObj
{
public:
    using Layout = CachelineLayout;
    CachelineObj(char *buf, size_t obj_size) : buf_(buf), obj_size_(obj_size)
    {
    }
    void init()
    {
        fill(0, {}, {});
    }
    Layout layout() const
    {
        return Layout{obj_size_};
    }
    void to_buf(char *to_buf)
    {
        layout().obj_to_buf(to_buf, buf_);
    }
    void fill(int fill, Layout::g_version_t gv, Layout::lock_t lock_val)
    {
        layout().obj_fill(buf_, fill, gv, lock_val);
    }
    void from_buf(const char *from_buf,
                  Layout::g_version_t gv,
                  Layout::lock_t lock_val)
    {
        layout().obj_from_buf(buf_, from_buf, gv, lock_val);
    }
    Layout::lock_t *lock_buf()
    {
        return (Layout::lock_t *) (buf_ + layout().lock_offset());
    }
    const Layout::lock_t *lock_buf() const
    {
        return (Layout::lock_t *) (buf_ + layout().lock_offset());
    }
    bool is_unlocked() const
    {
        return *lock_buf() == 0;
    }
    bool is_locked() const
    {
        return !is_unlocked();
    }

    bool consistent() const
    {
        auto gv = *(Layout::g_version_t *) (buf_ + layout().g_version_offset());
        auto cl_v = (Layout::cl_version_t) gv;
        for (size_t i = 0; i < layout().trunk_nr(); ++i)
        {
            auto *p_cl_v =
                (Layout::cl_version_t *) (buf_ + i * layout().cacheline_size());
            auto get_cl_v = *p_cl_v;
            if (get_cl_v != cl_v)
            {
                LOG(INFO) << "mismatch: "
                          << PRE(util::pre_hex(gv),
                                 util::pre_hex(cl_v),
                                 util::pre_hex(get_cl_v),
                                 i);
                return false;
            }
        }
        return true;
    }

private:
    char *buf_;
    size_t obj_size_;
};

// [V object] [V objet] [V object] [lock; V]
// NOTE: lock has to be 8-byte alinged. So put lock before V
// must place V before object: so that write can touch V first.
class Cacheline
{
public:
    using Layout = CachelineLayout;
    Cacheline(DSM *dsm,
              CoroContext *ctx,
              GlobalAddress obj_gaddr,
              size_t obj_size)
        : dsm_(dsm), ctx_(ctx), obj_gaddr_(obj_gaddr), obj_size_(obj_size)
    {
        obj_buf_ = dsm_->get_rdma_buffer(layout().total_size());
    }
    ~Cacheline()
    {
        dsm_->put_rdma_buffer(std::move(obj_buf_));
    }

    uint32_t *get_version_rdma_buffer()
    {
        return (uint32_t *) (obj_buf_.buffer + layout().g_version_offset());
    }
    GlobalAddress version_gaddr() const
    {
        return obj_gaddr_ + layout().g_version_offset();
    }
    GlobalAddress lock_gaddr() const
    {
        return obj_gaddr_ + layout().lock_offset();
    }

    GlobalAddress lock_version_gaddr() const
    {
        return lock_gaddr();
    }
    uint32_t *get_lock_rdma_buffer() const
    {
        return (uint32_t *) (obj_buf_.buffer + layout().lock_offset());
    }

    bool try_lock()
    {
        auto l = layout();
        // CAS lock and read version as a whole
        auto gaddr = lock_version_gaddr();
        auto size = l.lock_version_size();
        uint64_t *vl_ptr = (uint64_t *) get_version_rdma_buffer();
        Layout::lock_t *lock_ptr = get_lock_rdma_buffer();
        *lock_ptr = 0;  // unlock
        uint64_t swap = *vl_ptr;
        swap |= 1ull;  // lock
        dsm_->prepare_cas(gaddr,
                          size,
                          *vl_ptr,
                          0xffffffff00000000,
                          swap,
                          0xffffffff00000000,
                          vl_ptr,
                          false,
                          ctx_);
        dsm_->commit(ctx_);
        uint64_t got = *vl_ptr;
        auto lock_mask = 0xffffffff;
        if ((got & lock_mask) == 0)
        {
            // lock ok
            return true;
        }
        else
        {
            return false;
        }
    }

    void prepare_write(char *new_obj)
    {
        auto *p_g_v = (Layout::g_version_t *) get_version_rdma_buffer();
        auto expect_g_version = *p_g_v + 1;
        auto expect_lock_val = Layout::lock_t{};
        CachelineObj obj(obj_buf_.buffer, obj_size_);
        obj.from_buf(new_obj, expect_g_version, expect_lock_val);
        CHECK(obj.is_unlocked())
            << "** internal error: prepare_write not unlocking the obj";
        auto lock_val = *lock_buf();
        CHECK_EQ(lock_val, 0);
    }
    Layout::lock_t *lock_buf()
    {
        return (Layout::lock_t *) (obj_buf_.buffer + layout().lock_offset());
    }

    size_t lock_write_unlock(void *new_obj)
    {
        size_t try_nr = 0;
        while (true)
        {
            bool ok = try_lock();
            try_nr++;
            if (ok)
            {
                break;
            }
        }
        write_unlock(new_obj);
        return try_nr;
    }

    // CONTRACT: make sure it has been locked.
    void write_unlock(void *new_obj)
    {
        prepare_write((char *) new_obj);
        dsm_->prepare_write(
            obj_buf_.buffer, obj_gaddr_, layout().total_size(), false, ctx_);
        dsm_->commit(ctx_);
    }
    bool try_read(char *buf)
    {
        dsm_->prepare_read(
            obj_buf_.buffer, obj_gaddr_, layout().total_size(), false, ctx_);
        dsm_->commit(ctx_);

        CachelineObj obj(obj_buf_.buffer, obj_size_);
        if (obj.consistent())
        {
            obj.to_buf(buf);
            return true;
        }
        else
        {
            return false;
        }
    }
    size_t read(void *buf)
    {
        size_t try_nr = 0;
        while (true)
        {
            bool ok = try_read((char *) buf);
            try_nr++;
            if (ok)
            {
                return try_nr;
            }
        }
    }
    Layout layout() const
    {
        return Layout{obj_size_};
    }

private:
    DSM *dsm_;
    CoroContext *ctx_;
    GlobalAddress obj_gaddr_{};
    size_t obj_size_;

    Buffer obj_buf_;
};

enum Sync
{
    kCRC,
    kCOW,
    kCacheline,
    kRFR,
};

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using BLS = typename Base::BLS;
    Experiment()
    {
        DSMConfig config;
        config.worker_nr = 0;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();

        server_nid_ = dsm_->getClusterSize() - 1;
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }

    bool is_server() const
    {
        return dsm_->get_node_id() == server_nid_;
    }

    void on_start_bench(BLS &bls, const Config &conf) override
    {
        write_ok_.fill(0);
        write_nr_.fill(0);
        read_ok_.fill(0);
        read_nr_.fill(0);

        auto sync = conf.get<enum Sync>("sync");

        if (is_server())
        {
            auto obj_size = conf.get<size_t>("data_size");
            auto data_nr = conf.get<size_t>("data_nr");

            if (sync == Sync::kCRC)
            {
                for (size_t i = 0; i < data_nr; ++i)
                {
                    auto obj = get_local_crc(obj_size, i);
                    obj.init();
                }
            }
            else if (sync == Sync::kCOW)
            {
                for (size_t i = 0; i < data_nr; ++i)
                {
                    auto obj = get_local_cow(obj_size, i);
                    obj.init();
                }
            }
            else if (sync == Sync::kCacheline)
            {
                for (size_t i = 0; i < data_nr; ++i)
                {
                    auto obj = get_local_cl(obj_size, i);
                    obj.init();
                }
            }
            else if (sync == Sync::kRFR)
            {
                for (size_t i = 0; i < data_nr; ++i)
                {
                    auto obj = get_local_rfr(obj_size, i);
                    obj.init();
                }
            }
            else
            {
                LOG(FATAL) << "TODO:..." << PRE(sync);
            }
        }

        Base::on_start_bench(bls, conf);
    }
    void on_thread_start_bench(BLS &bls, const Config &conf) override
    {
        auto data_nr = conf.get<size_t>("data_nr");
        double z = conf.get<double>("z");
        // init generator
        std::unique_ptr<util::Generator> base_g;
        if (z == 0)
        {
            base_g = std::make_unique<util::UniformGenerator>(0, data_nr - 1);
        }
        else
        {
            base_g =
                std::make_unique<util::ZipfianGenerator>(0, data_nr - 1, z);
        }
        g_.current() = std::make_unique<util::ShuffleGenerator>(
            std::move(base_g), data_nr);
        Base::on_thread_start_bench(bls, conf);
    }
    void on_end_bench(const ResultRecord &r,
                      BLS &bls,
                      const Config &conf) override
    {
        LOG(INFO) << PRE(write_nr_);
        LOG(INFO) << PRE(write_ok_);
        LOG(INFO) << PRE(read_nr_);
        LOG(INFO) << PRE(read_ok_);
        Base::on_end_bench(r, bls, conf);
    }

    CRCObj get_local_crc(size_t obj_size, size_t obj_id)
    {
        auto total_size = CRC::Layout::total_size(obj_size);
        auto aligned_total_size = util::round_up_aligned(total_size, 64);
        char *base_addr = (char *) dsm_->get_base_addr();

        return CRCObj(base_addr + obj_id * aligned_total_size, obj_size);
    }
    RFRObj get_local_rfr(size_t obj_size, size_t obj_id)
    {
        auto total_size = RFR::Layout::total_size(obj_size);
        auto aligned_total_size = util::round_up_aligned(total_size, 64);
        char *base_addr = (char *) dsm_->get_base_addr();

        return RFRObj(base_addr + obj_id * aligned_total_size, obj_size);
    }

    RFR get_rfr(CoroContext *ctx, size_t obj_size, size_t obj_id)
    {
        auto aligned_total_size = util::round_up_aligned(obj_size + 8, 64);

        GlobalAddress gaddr;
        gaddr.nodeID = server_nid_;
        gaddr.offset = obj_id * aligned_total_size;

        // return gaddr;
        return RFR(dsm_.get(), ctx, gaddr, obj_size);
    }

    CRC get_crc(CoroContext *ctx, size_t obj_size, size_t obj_id)
    {
        auto total_size = CRC::Layout::total_size(obj_size);
        auto aligned_total_size = util::round_up_aligned(total_size, 64);

        GlobalAddress gaddr;
        gaddr.nodeID = server_nid_;
        gaddr.offset = obj_id * aligned_total_size;

        return CRC(dsm_.get(), ctx, gaddr, obj_size);
    }
    COWObj get_local_cow(size_t obj_size, size_t obj_id)
    {
        std::ignore = obj_size;
        auto ptr_size = COW::Layout::ptr_size();
        char *base_addr = (char *) dsm_->get_base_addr();
        return COWObj(base_addr + ptr_size * obj_id);
    }
    COW get_cow(CoroContext *ctx, size_t obj_size, size_t obj_id)
    {
        auto ptr_size = COW::Layout::ptr_size();
        GlobalAddress gaddr;
        gaddr.nodeID = server_nid_;
        gaddr.offset = obj_id * ptr_size;
        return COW(dsm_.get(), ctx, gaddr, obj_size);
    }
    CachelineObj get_local_cl(size_t obj_size, size_t obj_id)
    {
        auto total_size = Cacheline::Layout::total_size(obj_size);
        auto aligned_total_size = util::round_up_aligned(total_size, 64);
        char *base_addr = (char *) dsm_->get_base_addr();
        return CachelineObj(base_addr + obj_id * aligned_total_size, obj_size);
    }

    Cacheline get_cl(CoroContext *ctx, size_t obj_size, size_t obj_id)
    {
        auto total_size = Cacheline::Layout::total_size(obj_size);
        auto aligned_total_size = util::round_up_aligned(total_size, 64);
        GlobalAddress gaddr;
        gaddr.nodeID = server_nid_;
        gaddr.offset = obj_id * aligned_total_size;

        return Cacheline(dsm_.get(), ctx, gaddr, obj_size);
    }

    void run_cow(BLS &bls,
                 const Config &conf,
                 CoroContext &ctx,
                 ::bench::StopToken::pointer token)
    {
        size_t write_ratio = conf.get<size_t>("write_ratio");
        size_t thread_nr = conf.thread_nr();
        size_t writer_nr = thread_nr * write_ratio / 100.0;
        size_t data_size = conf.get<size_t>("data_size");
        bool is_writer = bls.id().select_thread(writer_nr);
        bool report_writer = conf.get<bool>("report_writer");

        std::optional<COWLayout::ptr_t> cached_ptr;

        OnePassBucketMonitor<uint64_t> cow_write_try_nr(0, 100, 1);

        while (!token->stop_requested())
        {
            auto obj_id = g_.current()->Next();
            auto cow = get_cow(&ctx, data_size, obj_id);
            if (is_writer)
            {
                // one CPU_write: random fill buffer
                // one RDMA_write: fill remote buffer
                char *buffer = cow.prepare_write();
                fast_pseudo_fill_buf(buffer, data_size);
                // try_nr * 1 RDMA_CAS
                size_t try_nr = cow.write();
                write_nr_.current() += try_nr;
                write_ok_.current()++;
                cow_write_try_nr.collect(try_nr);

                token->complete_task(report_writer);
            }
            else
            {
                // NOTE: don't provide cached object_addr, let it read twice
                // two RDMA_read: read ptr + read buffer
                if (cached_ptr && FLAGS_cow_use_cache)
                {
                    cow.read_direct(*cached_ptr);
                }
                else
                {
                    cow.read();
                }
                read_nr_.current()++;
                read_ok_.current()++;

                cached_ptr = *cow.ptr_buf();

                token->complete_task(!report_writer);
            }
        }
        LOG(INFO) << PRE(cow_write_try_nr);
    }
    void run_crc(BLS &bls,
                 const Config &conf,
                 CoroContext &ctx,
                 ::bench::StopToken::pointer token)
    {
        size_t write_ratio = conf.get<size_t>("write_ratio");
        size_t thread_nr = conf.thread_nr();
        size_t writer_nr = thread_nr * write_ratio / 100.0;
        size_t data_size = conf.get<size_t>("data_size");
        bool is_writer = bls.id().select_thread(writer_nr);
        bool report_writer = conf.get<bool>("report_writer");

        OnePassBucketMonitor<uint64_t> crc_write_try_nr(0, 100, 1);
        OnePassBucketMonitor<uint64_t> crc_read_try_nr(0, 100, 1);

        while (!token->stop_requested())
        {
            auto obj_id = g_.current()->Next();
            auto crc = get_crc(&ctx, data_size, obj_id);

            if (is_writer)
            {
                // CPU_write to random fill the buffer
                auto *buffer = crc.get_object_rdma_buf();
                fast_pseudo_fill_buf(buffer, data_size);
                // try_nr * RDMA_CAS for lock
                auto try_nr = crc.lock();
                write_nr_.current() += try_nr;
                crc_write_try_nr.collect(try_nr);
                // RDMA_write to update data, CRC and unlock
                crc.write_unlock();
                write_ok_.current()++;

                // LOG(INFO) << "writer: " << PRE(try_nr);

                token->complete_task(report_writer);
            }
            else
            {
                // auto [ok, buf] = crc.try_read();
                auto [try_nr, buf] = crc.read();
                read_nr_.current() += try_nr;
                read_ok_.current()++;
                crc_read_try_nr.collect(try_nr);

                // LOG(INFO) << "reader: " << PRE(try_nr);

                token->complete_task(!report_writer);
            }
        }
        LOG_IF(INFO, is_writer) << PRE(crc_write_try_nr);
        LOG_IF(INFO, !is_writer) << PRE(crc_read_try_nr);
    }
    void run_rfr(BLS &bls,
                 const Config &conf,
                 CoroContext &ctx,
                 ::bench::StopToken::pointer token)
    {
        size_t write_ratio = conf.get<size_t>("write_ratio");
        size_t thread_nr = conf.thread_nr();
        size_t writer_nr = thread_nr * write_ratio / 100.0;
        size_t data_size = conf.get<size_t>("data_size");
        bool is_writer = bls.id().select_thread(writer_nr);
        bool report_writer = conf.get<bool>("report_writer");

        while (!token->stop_requested())
        {
            auto obj_id = g_.current()->Next();
            auto rfr = get_rfr(&ctx, data_size, obj_id);
            if (is_writer)
            {
                // CPU_write to random fill the buffer
                // auto *buffer = crc.get_object_rdma_buf();
                auto *buffer = rfr.get_object_rdma_buf();
                fast_pseudo_fill_buf(buffer, data_size);
                // try_nr * RDMA_CAS for lock
                auto try_nr = rfr.lock();
                write_nr_.current() += try_nr;
                // RDMA_write to update data, CRC and unlock
                rfr.write_unlock();
                write_ok_.current()++;

                token->complete_task(report_writer);
            }
            else
            {
                // LOG(INFO) << "issue read: " << util::get_thread_id();
                auto [try_nr, buf] = rfr.read();
                read_nr_.current() += try_nr;
                read_ok_.current()++;
                // LOG(INFO) << "read ok: " << util::get_thread_id();

                token->complete_task(!report_writer);
            }
        }
    }
    void run_cacheline(BLS &bls,
                       const Config &conf,
                       CoroContext &ctx,
                       ::bench::StopToken::pointer token)
    {
        size_t write_ratio = conf.get<size_t>("write_ratio");
        size_t thread_nr = conf.thread_nr();
        size_t writer_nr = thread_nr * write_ratio / 100.0;
        size_t data_size = conf.get<size_t>("data_size");
        bool is_writer = bls.id().select_thread(writer_nr);
        bool report_writer = conf.get<bool>("report_writer");

        char *new_obj = (char *) malloc(data_size);

        while (!token->stop_requested())
        {
            auto obj_id = g_.current()->Next();
            auto cl = get_cl(&ctx, data_size, obj_id);

            if (is_writer)
            {
                fast_pseudo_fill_buf(new_obj, data_size);
                // nr * RDMA_CAS for lock
                // 1 * RDMA_write for update data and unlock
                auto nr = cl.lock_write_unlock(new_obj);
                write_nr_.current() += nr;
                write_ok_.current()++;

                token->complete_task(report_writer);
            }
            else
            {
                // nr * RDMA_read for getting consistent data
                auto nr = cl.read(new_obj);
                read_nr_.current() += nr;
                read_ok_.current()++;
                token->complete_task(!report_writer);
            }
        }

        free(new_obj);
    }

    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        auto dsm = get_dsm();
        auto sync = conf.get<enum Sync>("sync");
        if (sync == kCRC)
        {
            run_crc(bls, conf, ctx, token);
        }
        else if (sync == kCOW)
        {
            run_cow(bls, conf, ctx, token);
        }
        else if (sync == kCacheline)
        {
            run_cacheline(bls, conf, ctx, token);
        }
        else if (sync == kRFR)
        {
            // LOG(INFO) << "run_rfr";
            run_rfr(bls, conf, ctx, token);
        }
        else
        {
            LOG(FATAL) << "TODO";
        }
    }

private:
    DSM::pointer dsm_;

    Perthread<size_t> write_ok_;
    Perthread<size_t> write_nr_;
    Perthread<size_t> read_ok_;
    Perthread<size_t> read_nr_;

    Perthread<std::unique_ptr<util::Generator>> g_;
    size_t server_nid_{};
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    Experiment exp;

    bench::ConfigFactory f;
    // f.configure_thread_nr({1, 4, 8, 16});
    // f.configure_thread_nr({kMaxAppThread});
    f.configure_thread_nr({4, 16, kMaxAppThread});
    f.configure_coro_nr({3});
    // f.add_option<size_t>("data_size", {4_KB});
    f.add_option<size_t>("data_size", {4_KB});
    // f.add_option<size_t>("data_size", {2_MB});
    // f.add_option<size_t>("data_size", {64, kInternalPageSize});
    // f.add_option<double>("z", {FLAGS_z});
    f.add_option<double>("z", {0});
    // f.add_option<size_t>("data_nr", {1024});
    f.add_option<size_t>("data_nr", {1024});
    // f.add_option<size_t>("write_ratio", {100, 80, 50, 0});
    f.add_option<size_t>("write_ratio", {0});
    // f.add_option<enum Sync>("sync", {kCOW, kCRC, kCacheline});
    // f.add_option<enum Sync>("sync", {kCRC, kCacheline});
    // f.add_option<enum Sync>("sync", {kCacheline, kCOW, kCRC});
    f.add_option<enum Sync>("sync", {kRFR, kCRC});
    // f.add_option<enum Sync>("sync", {kCOW});
    // f.add_option<bool>("report_writer", {true, false});
    // f.add_option<bool>("report_writer", {true, false});
    f.add_option<bool>("report_writer", {false});
    auto configs = f.generate_configs();

    exp.configure_monitor(400ms, 8);
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}

// Skewness(Zipfian) or Uniform
// Large (4KB) or small (64B)
// Write-intensive(80), Write-heavy(50), RO (0)

// Uniform: