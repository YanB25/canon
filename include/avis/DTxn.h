#pragma once

#include <city.h>

#include "DSM.h"
#include "GlobalAddress.h"
#include "avis/handle.h"

namespace avis
{

class DTxn
{
public:
    struct RW
    {
        Buffer rdma_buf;
        size_t size;
    };
    constexpr static bool kReport = false;
    DTxn(DSM::pointer dsm,
         GlobalAddress locks,
         size_t lock_size,
         GlobalAddress data,
         size_t data_size,
         GlobalAddress bitmap,
         size_t bitmap_size,
         CoroContext *ctx)
        : dsm_(dsm),
          locks_(locks),
          lock_size_(lock_size),
          data_(data),
          data_size_(data_size),
          bitmap_(bitmap),
          bitmap_size_(bitmap_size),
          ctx_(ctx)
    {
    }
    GlobalAddress alloc(size_t size)
    {
        auto bit_id = bitmap_idx_;
        bitmap_idx_++;
        auto block_id = bit_id / 64;
        auto bit_offset = bit_id % 64;
        rdma_faa(bitmap_ + 8 * block_id, 1ull << bit_offset);
        CHECK_LT(bit_id / 8, bitmap_size_);

        auto gaddr = data_ + bit_id * 800;
        CHECK_LE(size, 800);
        CHECK_LE(gaddr, data_ + data_size_);
        return gaddr;
    }

    char *rdma_read(GlobalAddress raddr, size_t size)
    {
        auto &rw = read_set_[raddr];
        rw.size = size;
        rw.rdma_buf = dsm_->get_rdma_buffer(size);
        dsm_->prepare_read(rw.rdma_buf.buffer, raddr, size, false, ctx_);
        LOG_IF(INFO, kReport) << "[dtxn] read " << size;
        dsm_->commit(ctx_);
        return rw.rdma_buf.buffer;
    }
    void rdma_faa(GlobalAddress raddr, uint64_t add_val)
    {
        char *r = rdma_read(raddr, 8);
        uint64_t old_val = *(uint64_t *) r;
        uint64_t new_val = old_val + add_val;
        rdma_write(raddr, (char *) &new_val, 8);
    }
    void rdma_cas(GlobalAddress raddr, uint64_t compare, uint64_t swap)
    {
        char *r = rdma_read(raddr, 8);
        uint64_t got = *(uint64_t *) r;
        if (got == compare)
        {
            rdma_write(raddr, (char *) &swap, 8);
        }
    }
    void rdma_write(GlobalAddress raddr, const char *buf, size_t size)
    {
        auto &rw = write_set_[raddr];
        rw.size = size;
        rw.rdma_buf = dsm_->get_rdma_buffer(size);
        memcpy(rw.rdma_buf.buffer, buf, size);
    }
    void do_acquire_lock(const std::set<uint64_t> &lock_ids)
    {
        constexpr size_t kParallel = 16;
        size_t ongoing = 0;

        std::vector<Buffer> bufs;
        for (auto id : lock_ids)
        {
            bufs.emplace_back(dsm_->get_rdma_buffer(8));
            auto &buf = bufs.back();

            dsm_->prepare_cas(locks_ + id * 8,
                              8,
                              0,
                              0xffffffffffffffff,
                              1,
                              0xffffffffffffffff,
                              buf.buffer,
                              false,
                              ctx_);
            LOG_IF(INFO, kReport) << "[dtxn] CAS";
            ongoing++;
            if (ongoing >= kParallel)
            {
                dsm_->commit(ctx_);
                ongoing = 0;
            }
        }
        if (ongoing)
        {
            dsm_->commit(ctx_);
            ongoing = 0;
        }

        for (auto &&buf : bufs)
        {
            dsm_->put_rdma_buffer(std::move(buf));
        }
    }
    void do_release_lock(const std::set<uint64_t> &lock_ids)
    {
        constexpr size_t kParallel = 16;
        size_t ongoing = 0;

        auto buf = dsm_->get_rdma_buffer(8);
        memset(buf.buffer, 0, 8);
        for (auto id : lock_ids)
        {
            dsm_->prepare_write(buf.buffer, locks_ + id * 8, 8, false, ctx_);
            ongoing++;
            if (ongoing >= kParallel)
            {
                dsm_->commit(ctx_);
                ongoing = 0;
            }
        }
        if (ongoing)
        {
            dsm_->commit(ctx_);
            ongoing = 0;
        }
        dsm_->put_rdma_buffer(std::move(buf));
    }

    void commit()
    {
        // phase_one: lock write set
        std::set<uint64_t> hold_locks;
        for (const auto &[gaddr, rw] : write_set_)
        {
            uint64_t hash = CityHash64((char *) &gaddr.val, sizeof(gaddr));
            uint64_t lock_id = hash % lock_size_;
            hold_locks.insert(lock_id);
        }
        do_acquire_lock(hold_locks);

        // phase two: reread
        constexpr size_t kParallel = 8;
        std::vector<Buffer> reread_bufs;
        size_t ongoing_nr = 0;
        for (const auto &[gaddr, rw] : read_set_)
        {
            reread_bufs.emplace_back(dsm_->get_rdma_buffer(rw.size));
            auto &reread_buf = reread_bufs.back();

            dsm_->prepare_read(reread_buf.buffer, gaddr, rw.size, false, ctx_);
            ongoing_nr++;
            if (ongoing_nr >= kParallel)
            {
                dsm_->commit(ctx_);
                ongoing_nr = 0;
            }
        }
        if (ongoing_nr)
        {
            dsm_->commit(ctx_);
            ongoing_nr = 0;
        }
        for (auto &&buf : reread_bufs)
        {
            dsm_->put_rdma_buffer(std::move(buf));
        }

        // phase three: write
        for (const auto &[gaddr, rw] : write_set_)
        {
            dsm_->prepare_write(
                rw.rdma_buf.buffer, gaddr, rw.size, false, ctx_);
            ongoing_nr++;
            if (ongoing_nr >= kParallel)
            {
                dsm_->commit(ctx_);
                ongoing_nr = 0;
            }
        }
        if (ongoing_nr)
        {
            dsm_->commit(ctx_);
            ongoing_nr = 0;
        }

        do_release_lock(hold_locks);

        for (auto &[gaddr, rw] : read_set_)
        {
            dsm_->put_rdma_buffer(std::move(rw.rdma_buf));
        }
        read_set_.clear();
        for (auto &[gaddr, rw] : write_set_)
        {
            dsm_->put_rdma_buffer(std::move(rw.rdma_buf));
        }
        write_set_.clear();
    }

private:
    DSM::pointer dsm_;
    GlobalAddress locks_;
    size_t lock_size_;
    GlobalAddress data_;
    size_t data_size_;
    GlobalAddress bitmap_;
    size_t bitmap_size_;
    CoroContext *ctx_;

    std::unordered_map<GlobalAddress, RW> read_set_;
    std::unordered_map<GlobalAddress, RW> write_set_;

    size_t bitmap_idx_{0};
};

};  // namespace avis