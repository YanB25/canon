#pragma once

#include <city.h>

#include "CoroContext.h"
#include "DSM.h"
#include "GlobalAddress.h"

namespace avis
{
template <typename T>
class Publisher
{
public:
    constexpr static bool kReport = false;
    Publisher(DSM::pointer dsm,
              GlobalAddress area,
              size_t size,
              CoroContext *ctx)
        : dsm_(dsm), area_(area), size_(size), ctx_(ctx)
    {
        CHECK(!area_.is_null());
        rdma_buf_ = dsm_->get_rdma_buffer(size);
        memset(rdma_buf_.buffer, 0, size);
    }
    void set_ctx(CoroContext *ctx)
    {
        ctx_ = ctx;
    }
    void init()
    {
        memset(rdma_buf_.buffer, 0, size_);
        dsm_->prepare_write(rdma_buf_.buffer, area_, size_, false, ctx_);
        dsm_->commit(ctx_);
    }
    ~Publisher()
    {
        dsm_->put_rdma_buffer(std::move(rdma_buf_));
    }

    bool publish(const T &t)
    {
        check_update();

        dsm_->prepare_faa(area_,
                          sizeof(uint64_t),
                          1,
                          0 /* field boundary */,
                          rdma_buf_.buffer,
                          false,
                          ctx_);
        dsm_->commit(ctx_);
        uint64_t got_index = *(uint64_t *) rdma_buf_.buffer;
        LOG_IF(INFO, kReport)
            << "[publisher] publish " << util::pre(t) << " at " << got_index;
        check_update(got_index);

        GlobalAddress target_location =
            area_ + sizeof(uint64_t) + got_index * sizeof(T);
        if (target_location >= area_ + size_)
        {
            return false;
        }

        auto *ptr = object(got_index);
        *ptr = t;
        dsm_->prepare_write(
            (char *) ptr, target_location, sizeof(T), false, ctx_);
        DCHECK_LT(target_location, area_ + size_);
        dsm_->commit(ctx_);
        return true;
    }
    // return whether there is update
    bool check_update()
    {
        dsm_->prepare_read(
            rdma_buf_.buffer, area_, sizeof(uint64_t), false, ctx_);
        dsm_->commit(ctx_);
        uint64_t got_size = *(uint64_t *) rdma_buf_.buffer;
        return check_update(got_size);
    }
    bool check_update(size_t new_size)
    {
        if (new_size != cached_size_)
        {
            LOG_IF(INFO, kReport) << "[publisher] renew size from "
                                  << cached_size_ << " to " << new_size;
            prepare_read_meta(cached_size_, new_size);
            dsm_->commit(ctx_);

            cached_size_ = new_size;
            return true;
        }
        return false;
    }
    size_t size() const
    {
        return cached_size_;
    }
    size_t max_nr() const
    {
        return (size_ - sizeof(uint64_t)) / sizeof(T);
    }

    const T *object(size_t ith) const
    {
        if (ith >= max_nr())
        {
            return nullptr;
        }
        T *first = (T *) (rdma_buf_.buffer + sizeof(uint64_t));
        return &first[ith];
    }
    T *object(size_t ith)
    {
        if (ith >= max_nr())
        {
            return nullptr;
        }
        T *first = (T *) (rdma_buf_.buffer + sizeof(uint64_t));
        return &first[ith];
    }

private:
    DSM::pointer dsm_;
    GlobalAddress area_;
    size_t size_;
    CoroContext *ctx_;

    Buffer rdma_buf_;

    size_t cached_size_{};

    void prepare_read_meta(size_t begin, size_t end)
    {
        size_t begin_offset = sizeof(uint64_t) + begin * sizeof(T);
        size_t size = (end - begin) * sizeof(T);
        if (size == 0)
        {
            return;
        }
        size = std::min(size, size_ - begin_offset);

        CHECK_LE(begin_offset + size, size_) << "** overflowed";

        LOG_IF(INFO, kReport) << "[publisher] prepare read at idx " << begin
                              << " for size " << end - begin;
        dsm_->prepare_read((char *) rdma_buf_.buffer + begin_offset,
                           area_ + begin_offset,
                           size,
                           false,
                           ctx_);
    }
};
};  // namespace avis
