#pragma once
/**
 * ARC (atomic reference counting) is an era-based non-blocking algorithm for
 * reference count maintenance, following Mingxing Zhang, etc, Partial Failure
 * Resilient Memory Management System for (CXL-based) Distributed Shared Memory,
 * SOSP'23.
 */

#include <iostream>

#include "./ptl.h"
#include "CoroContext.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "Metrics.h"

namespace avis::arc
{
using id_t = uint16_t;
using era_t = uint64_t;
using small_era_t = uint32_t;
using cnt_t = uint16_t;
struct Header
{
    id_t lcid;
    small_era_t lera;
    cnt_t ref_cnt;
} __attribute__((packed));
static_assert(sizeof(Header) == sizeof(uint64_t));
enum Op
{
    kAttach,
    kDetach,
};
struct Redo
{
    Op op;
    era_t cur_era;
    GlobalAddress ref;
    GlobalAddress refed;
    cnt_t saved_ref_cnt;
};

inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::avis::arc::Header &v)
{
    os << "{Header ";
    os << "lcid: " << util::pre(v.lcid);
    os << ", lera: " << util::pre(v.lera);
    os << ", ref_cnt: " << util::pre(v.ref_cnt);
    os << "}";
    return os;
}

class ARC
{
public:
    ARC(DSM::pointer dsm,
        id_t M,
        id_t cid,
        GlobalAddress meta,
        size_t meta_size,
        std::shared_ptr<avis::PTL> ptl,
        CoroContext *ctx)
        : dsm_(dsm),
          M_(M),
          cid_(cid),
          meta_(meta),
          meta_size_(meta_size),
          ptl_(ptl),
          ctx_(ctx)
    {
        auto require_meta = M * M * sizeof(era_t);
        CHECK_GE(meta_size, require_meta)
            << PRE(M) << ", required " << util::pre_byte(require_meta);
    }
    void init()
    {
        auto rdma_buf = dsm_->get_rdma_buffer(meta_size_);
        memset(rdma_buf.buffer, 0, meta_size_);
        dsm_->prepare_write(rdma_buf.buffer, meta_, meta_size_, false, ctx_);
        dsm_->commit(ctx_);

        dsm_->put_rdma_buffer(std::move(rdma_buf));
    }

    /**
     * Attach the reference at ref to the data block at refed.
     * Essentially there are two main steps:
     * 1) refed->header.ref_cnt++; // ModifyRefCnt: Increase ref count.
     * 2) *ref = refed; // ModifyRef: Link the reference
     */
    /**
     * 1) read saved
     * 2) read Era[client.cid][saw_cid]
     * 3) if (...) write Era[client.cid][saw_cid]
     *    read Era[client.cid][client.cid]
     * 4) Log Redo, CAS header.
     * 5) *ref = refed; Era[cid][cid]++
     */
    void AttachReference(GlobalAddress ref, GlobalAddress refed)
    {
        while (true)
        {
            auto hdr_buf = dsm_->get_rdma_buffer(sizeof(Header));
            dsm_->prepare_read(hdr_buf.buffer,
                               refed - sizeof(Header),
                               sizeof(Header),
                               false,
                               ctx_);
            rm_.read(sizeof(Header));
            dsm_->commit(ctx_);
            Header saved = *(Header *) hdr_buf.buffer;
            dsm_->put_rdma_buffer(std::move(hdr_buf));

            auto saw_cid = saved.lcid;
            auto saw_era = saved.lera;

            auto era = read_era(cid_, saw_cid);
            auto write_era_buf = dsm_->get_rdma_buffer(sizeof(era_t));
            if (era < saw_era)
            {
                write_era_async(cid_, saw_cid, saw_era, write_era_buf);
                rm_.write(sizeof(Header));
            }
            auto cur_era = read_era(cid_, cid_);
            // this should commit
            dsm_->put_rdma_buffer(std::move(write_era_buf));

            Redo redo{
                .op = kAttach,
                .cur_era = cur_era,
                .ref = ref,
                .refed = refed,
                .saved_ref_cnt = saved.ref_cnt,
            };
            // TODO async?
            auto log_buf = dsm_->get_rdma_buffer(sizeof(redo));
            ptl_->log_async(&redo, sizeof(redo), log_buf);
            rm_.write(sizeof(redo));

            auto newh = Header{
                .lcid = cid_,
                .lera = (small_era_t) cur_era,
                .ref_cnt = (cnt_t) (saved.ref_cnt + 1),
            };

            // while clause
            // LOG(INFO) << "CASing to " << (refed - sizeof(Header))
            //           << " with new header " << newh << " (old: " << saved
            //           << ")";
            auto cas_buf = dsm_->get_rdma_buffer(sizeof(Header));
            dsm_->prepare_cas(refed - sizeof(Header),
                              sizeof(Header),
                              *(uint64_t *) &saved,
                              0xffffffffffffffff,
                              *(uint64_t *) &newh,
                              0xffffffffffffffff,
                              cas_buf.buffer,
                              false,
                              ctx_);
            dsm_->commit(ctx_);
            dsm_->put_rdma_buffer(std::move(cas_buf));
            dsm_->put_rdma_buffer(std::move(log_buf));

            uint64_t old_val = *(uint64_t *) cas_buf.buffer;
            if (old_val == *(uint64_t *) &saved)
            {
                // CAS okay
                rm_.cas(sizeof(Header), true);
                break;
            }
            else
            {
                rm_.cas(sizeof(Header), false);
            }
        }

        auto rdma_buf = dsm_->get_rdma_buffer(sizeof(GlobalAddress));
        memcpy(rdma_buf.buffer, &refed.val, sizeof(GlobalAddress));

        dsm_->prepare_write(
            rdma_buf.buffer, ref, sizeof(GlobalAddress), false, ctx_);
        rm_.write(sizeof(GlobalAddress));

        auto faa_buf = dsm_->get_rdma_buffer(sizeof(era_t));
        dsm_->prepare_faa(era_gaddr(cid_, cid_),
                          sizeof(era_t),
                          1,
                          0,
                          faa_buf.buffer,
                          false,
                          ctx_);
        rm_.faa(sizeof(era_t));

        dsm_->commit(ctx_);

        dsm_->put_rdma_buffer(std::move(rdma_buf));
        dsm_->put_rdma_buffer(std::move(faa_buf));
    }

    void DetachReference(GlobalAddress ref, GlobalAddress refed)
    {
        while (true)
        {
            auto hdr_buf = dsm_->get_rdma_buffer(sizeof(Header));
            dsm_->prepare_read(hdr_buf.buffer,
                               refed - sizeof(Header),
                               sizeof(Header),
                               false,
                               ctx_);
            rm_.read(sizeof(Header));
            dsm_->commit(ctx_);
            Header saved = *(Header *) hdr_buf.buffer;
            dsm_->put_rdma_buffer(std::move(hdr_buf));

            auto saw_cid = saved.lcid;
            auto saw_era = saved.lera;

            auto era = read_era(cid_, saw_cid);
            auto write_era_buf = dsm_->get_rdma_buffer(sizeof(era_t));
            if (era < saw_era)
            {
                write_era_async(cid_, saw_cid, saw_era, write_era_buf);
                rm_.write(sizeof(Header));
            }
            auto cur_era = read_era(cid_, cid_);
            // this should commit
            dsm_->put_rdma_buffer(std::move(write_era_buf));

            Redo redo{
                .op = kDetach,
                .cur_era = cur_era,
                .ref = ref,
                .refed = refed,
                .saved_ref_cnt = saved.ref_cnt,
            };
            // TODO async?
            auto log_buf = dsm_->get_rdma_buffer(sizeof(redo));
            ptl_->log_async(&redo, sizeof(redo), log_buf);
            rm_.write(sizeof(redo));

            auto newh = Header{
                .lcid = cid_,
                .lera = (small_era_t) cur_era,
                .ref_cnt = (cnt_t) (saved.ref_cnt - 1),
            };

            // while clause
            LOG(INFO) << "CASing to " << (refed - sizeof(Header))
                      << " with new header " << newh << " (old: " << saved
                      << ")";
            auto cas_buf = dsm_->get_rdma_buffer(sizeof(Header));
            dsm_->prepare_cas(refed - sizeof(Header),
                              sizeof(Header),
                              *(uint64_t *) &saved,
                              0xffffffffffffffff,
                              *(uint64_t *) &newh,
                              0xffffffffffffffff,
                              cas_buf.buffer,
                              false,
                              ctx_);
            dsm_->commit(ctx_);
            dsm_->put_rdma_buffer(std::move(cas_buf));
            dsm_->put_rdma_buffer(std::move(log_buf));

            uint64_t old_val = *(uint64_t *) cas_buf.buffer;
            if (old_val == *(uint64_t *) &saved)
            {
                // CAS okay
                rm_.cas(sizeof(Header), true);
                break;
            }
            else
            {
                rm_.cas(sizeof(Header), false);
            }
        }

        auto rdma_buf = dsm_->get_rdma_buffer(sizeof(GlobalAddress));
        memcpy(rdma_buf.buffer, &refed.val, sizeof(GlobalAddress));

        dsm_->prepare_write(
            rdma_buf.buffer, ref, sizeof(GlobalAddress), false, ctx_);
        rm_.write(sizeof(GlobalAddress));

        auto faa_buf = dsm_->get_rdma_buffer(sizeof(era_t));
        dsm_->prepare_faa(era_gaddr(cid_, cid_),
                          sizeof(era_t),
                          1,
                          0,
                          faa_buf.buffer,
                          false,
                          ctx_);
        rm_.faa(sizeof(era_t));

        dsm_->commit(ctx_);

        dsm_->put_rdma_buffer(std::move(rdma_buf));
        dsm_->put_rdma_buffer(std::move(faa_buf));
    }

    auto metrics() const
    {
        return rm_;
    }

    era_t read_era(size_t row, size_t col)
    {
        auto rdma_buf = dsm_->get_rdma_buffer(sizeof(era_t));
        auto ith = row * M_ + col;
        auto offset = ith * sizeof(era_t);
        dsm_->prepare_read(
            rdma_buf.buffer, meta_ + offset, sizeof(era_t), false, ctx_);
        rm_.read(sizeof(era_t));
        dsm_->commit(ctx_);
        auto ret = *(era_t *) rdma_buf.buffer;
        dsm_->put_rdma_buffer(std::move(rdma_buf));
        return ret;
    }
    void write_era_async(size_t row, size_t col, era_t val, Buffer &rdma_buf)
    {
        memcpy(rdma_buf.buffer, &val, sizeof(val));
        auto ith = row * M_ + col;
        auto offset = ith * sizeof(era_t);
        dsm_->prepare_write(
            rdma_buf.buffer, meta_ + offset, sizeof(era_t), false, ctx_);
        rm_.write(sizeof(era_t));
    }
    void write_era(size_t row, size_t col, era_t val)
    {
        auto rdma_buf = dsm_->get_rdma_buffer(sizeof(era_t));
        write_era_async(row, col, val, rdma_buf);
        dsm_->commit(ctx_);
        dsm_->put_rdma_buffer(std::move(rdma_buf));
    }
    GlobalAddress era_gaddr(size_t row, size_t col)
    {
        auto ith = row * M_ + col;
        auto offset = ith * sizeof(era_t);
        return meta_ + offset;
    }

    void report()
    {
        auto rdma_buf = dsm_->get_rdma_buffer(meta_size_);
        dsm_->prepare_read(rdma_buf.buffer, meta_, meta_size_, false, ctx_);
        dsm_->commit(ctx_);

        era_t *pera = (era_t *) rdma_buf.buffer;

        std::vector<std::vector<uint64_t>> g_era;

        for (size_t row = 0; row < M_; ++row)
        {
            g_era.emplace_back();
            for (size_t col = 0; col < M_; ++col)
            {
                auto ith = row * M_ + col;
                auto era = pera[ith];
                g_era.back().emplace_back(era);
            }
        }
        LOG(INFO) << PRE(g_era);
        dsm_->put_rdma_buffer(std::move(rdma_buf));
    }

    void report(GlobalAddress gaddr)
    {
        auto hdr_buf = dsm_->get_rdma_buffer(sizeof(Header));
        dsm_->prepare_read(hdr_buf.buffer,
                           gaddr - sizeof(Header),
                           sizeof(Header),
                           false,
                           ctx_);
        dsm_->commit(ctx_);
        LOG(INFO) << *(Header *) hdr_buf.buffer;
        dsm_->put_rdma_buffer(std::move(hdr_buf));
    }

private:
    DSM::pointer dsm_;
    id_t M_;
    id_t cid_;
    GlobalAddress meta_;
    size_t meta_size_;
    std::shared_ptr<avis::PTL> ptl_;
    CoroContext *ctx_;

    RemoteMetrics rm_;
};
}  // namespace avis::arc