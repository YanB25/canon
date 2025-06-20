#pragma once

#include <math.h>

#include <array>
#include <cmath>

#include "./buddy_layout.h"
#include "./config.h"
#include "./debug.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "Metrics.h"
#include "avis/ptl.h"
#include "range/v3/view/span.hpp"
#include "util/Coro.h"
#include "util/Hexdump.hpp"
#include "util/IRdmaAdaptor.h"
#include "util/PerformanceReporter.h"
#include "util/Rand.h"
#include "util/Util.h"
#include "util/bits.h"
#include "util/thread_id.h"

namespace avis
{
extern util::TL_History<HisRecord> buddy_history_;

/**
 * BuddyAllocator is an allocator with buddy algorithm.
 * @param meta the address of the meta data.
 */
class BuddyAllocator
{
public:
    constexpr static bool kReport = false;
    constexpr static bool kEnableHistory = false;
    constexpr static bool kReportLatency = false;
    // bool kReport = false;
    BuddyAllocator(DSM::pointer dsm,
                   GlobalAddress meta_addr,
                   size_t meta_size,
                   GlobalAddress page_addr,
                   size_t total_size,
                   size_t page_size,
                   std::shared_ptr<PTL> ptl,
                   CoroContext *ctx)
        : dsm_(dsm),
          meta_addr_(meta_addr),
          page_addr_(page_addr),
          page_size_(page_size),
          ptl_(ptl),
          page_nr_(total_size / page_size_),
          ctx_(ctx),
          buddy_(total_size, page_size)
    {
        CHECK_EQ(total_size % page_size, 0);
        CHECK(util::is_power_of_two(page_nr_));
        CHECK_GT(page_nr_, 1);
        CHECK(util::is_aligned(page_addr.offset, page_size))
            << "** page_addr " << page_addr << " is not aligned to page_size "
            << page_size;
        CHECK(util::is_aligned(meta_addr.offset, 8))
            << "** meta_addr is not aligned to 8 (need to perform CAS)";
        depth_ = log2(page_nr_);
        node_nr_ = pow(2, depth_ + 1) - 1;

        meta_rdma_buf_ = dsm_->get_rdma_buffer(meta_bytes());
        memset(meta_rdma_buf_.buffer, 0, meta_rdma_buf_.size);
        cas_rdma_buf_ = dsm_->get_rdma_buffer(32);  // give you enough
        memset(cas_rdma_buf_.buffer, 0, cas_rdma_buf_.size);

        CHECK_GE(meta_size, meta_bytes()) << "** Provide a larger meta";

        read_meta();
    }
    void reset()
    {
        memset(meta_rdma_buf_.buffer, 0, meta_rdma_buf_.size);
        memset(cas_rdma_buf_.buffer, 0, cas_rdma_buf_.size);
    }
    void print_meta() const
    {
        LOG(INFO) << util::BitsView(meta_rdma_buf_.buffer, meta_bytes());
    }
    auto metrics() const
    {
        return std::make_pair(am_, rm_);
    }

    auto dump_latency() const
    {
        return lat_;
    }

    GlobalAddress get_free_pages(unsigned long order)
    {
        if (unlikely(avis::Config::ins().buddy_has_crashed()))
        {
            return GlobalAddress::Null();
        }

        ChronoTimer timer(kReportLatency);
        if constexpr (kEnableHistory)
        {
            trace_ = fast_pseudo_rand_int();
        }
        GlobalAddress ret;
        auto mode = Config::ins().buddy_mode();
        if (mode == BuddyMode::kBoundedRand)
        {
            ret = get_free_pages_bounded_random(order);
        }
        else if (mode == BuddyMode::kRand)
        {
            ret = get_free_pages_random(order);
        }
        else
        {
            CHECK_EQ(mode, BuddyMode::kSeq);
            ret = get_free_pages_seq(order);
        }

        if (likely(!ret.is_null()))
        {
            if constexpr (kReportLatency)
            {
                auto ns = timer.pin();
                // LOG(INFO) << "order: " << order
                //           << ", time: " << util::pre_ns(ns);
                auto it = lat_.find(order);
                if (unlikely(it == lat_.end()))
                {
                    auto min = std::chrono::nanoseconds(0ns).count();
                    auto max = std::chrono::nanoseconds(40ms).count();
                    auto rng = std::chrono::nanoseconds(1us).count();
                    it = lat_.emplace(
                                 order,
                                 OnePassBucketMonitor<uint64_t>(min, max, rng))
                             .first;
                }
                it->second.collect(ns);
            }

            am_.record_alloc(4_KB * (1ull << order));
            DCHECK(contains(ret)) << "** internal corruption: addr " << ret
                                  << " not belong to me.";
            if constexpr (kEnableHistory)
            {
                auto size = (1ull << order) * page_size();
                buddy_history_.current().add(HisRecord{
                    .is_alloc = true,
                    .raddr = ret,
                    .size = size,
                    .tid = (int) util::get_thread_id(),
                    .cid = ctx_ ? ctx_->coro_id() : kNotACoro,
                });
                auto node_id = cal_node_id(ret, order);
                auto bit_id = node_id_to_bit_offset(node_id);
                LOG_IF(INFO, kReport)
                    << "[buddy] allocate " << ret << " size " << size
                    << " at page_id: " << raddr_to_page_id(ret)
                    << ", node_id: " << node_id << ", bit_id " << bit_id << " "
                    << (void *) trace_;
            }
        }
        return ret;
    }

    bool contains(GlobalAddress raddr) const
    {
        if (raddr < page_addr_)
        {
            return false;
        }

        auto offset = raddr.offset - page_addr_.offset;
        DCHECK_EQ(offset % page_size_, 0);
        auto page_id = offset / page_size_;
        if (page_id >= page_nr_)
        {
            return false;
        }
        return true;
    }
    void put_free_pages(GlobalAddress addr, unsigned int order)
    {
        if constexpr (kEnableHistory)
        {
            trace_ = fast_pseudo_rand_int();
        }

        if (unlikely(addr.is_null()))
        {
            return;
        }

        if constexpr (kEnableHistory)
        {
            size_t size = (1ull << order) * page_size();
            buddy_history_.current().add(HisRecord{
                .is_alloc = false,
                .raddr = addr,
                .size = size,
                .tid = (int) util::get_thread_id(),
                .cid = ctx_ ? ctx_->coro_id() : kNotACoro,
            });
            LOG(INFO) << "[buddy] free " << addr << " with size " << size;
        }

        DCHECK(contains(addr)) << "** addr " << addr << " not belong to me.";

        auto node_id = cal_node_id(addr, order);
        LOG_IF(INFO, kReport)
            << "[buddy] put_free_pages(" << addr << ") node_id: " << node_id
            << " " << (void *) trace_;
        do_free_page_at(node_id);

        auto pn = page_nr_at(node_id);
        am_.record_dealloc(4_KB * pn);
    }
    ~BuddyAllocator()
    {
        dsm_->put_rdma_buffer(std::move(meta_rdma_buf_));
        dsm_->put_rdma_buffer(std::move(cas_rdma_buf_));
    }

    constexpr size_t page_nr() const
    {
        return page_nr_;
    }
    constexpr size_t page_size() const
    {
        return page_size_;
    }
    constexpr size_t depth() const
    {
        return depth_;
    }
    constexpr size_t node_nr() const
    {
        return node_nr_;
    }

    // [begin, end) of node_id
    using PageRun = std::pair<int, int>;
    void explain()
    {
        auto [allocated, conflict] = get_allocated();
        CHECK_EQ(conflict.size(), 0) << "** " << PRE(conflict);
        for (const auto &[begin, end] : allocated)
        {
            LOG(INFO) << "[buddy] allocated: [" << begin << ", " << end << ")";
        }
    }
    std::pair<std::vector<PageRun>, std::vector<PageRun>> get_allocated() const
    {
        std::vector<PageRun> allocated;
        std::vector<PageRun> conflict;
        auto root_id = 0;
        do_get_allocated(allocated, conflict, root_id);
        return {allocated, conflict};
    }
    struct Dump
    {
        size_t total_page_nr_;
        size_t page_size_;
        size_t alloc_nr_;
        size_t allocated_page_nr_;
        size_t total_page_nr() const
        {
            return total_page_nr_;
        }
        size_t page_size() const
        {
            return page_size_;
        }
        size_t alloc_nr() const
        {
            return alloc_nr_;
        }
        size_t allocated_bytes() const
        {
            return allocated_page_nr_ * page_size_;
        }
        size_t total_bytes() const
        {
            return total_page_nr_ * page_size_;
        }
        size_t avail_bytes() const
        {
            return total_bytes() - allocated_bytes();
        }
    };
    Dump dump() const
    {
        auto [a, _] = get_allocated();
        Dump ret;
        ret.total_page_nr_ = page_nr();
        ret.page_size_ = page_size();
        ret.alloc_nr_ = 0;
        ret.allocated_page_nr_ = 0;
        for (const auto &[begin, end] : a)
        {
            ret.alloc_nr_++;
            auto page_nr = end - begin;
            ret.allocated_page_nr_ += page_nr;
        }
        return ret;
    }
    struct FragDump
    {
        std::map<size_t, size_t> frag;
        double index;
    };

    FragDump dump_fragmentation() const
    {
        FragDump ret;
        for (unsigned order = 0; order <= depth(); ++order)
        {
            auto nr = available_alloc_nr(order);
            ret.frag[order] = nr;
        }
        ret.index = 0;
        if (!ret.frag.empty())
        {
            size_t max_order = 0;
            for (const auto &[order, nr] : ret.frag)
            {
                if (nr)
                {
                    max_order = std::max(max_order, order);
                }
            }
            auto largest = page_size() * (1ull << max_order);
            double frag_idx = 1 - 1.0 * largest / (page_nr_ * page_size_);
            ret.index = frag_idx;
            // LOG(INFO) << PRE(max_order, largest, page_nr_, page_size_);
        }
        return ret;
    }

    void report_fragmentation() const
    {
        LOG(INFO) << util::pre(dump_fragmentation());
    }
    size_t available_alloc_nr(unsigned int order) const
    {
        auto reversed_depth = order;
        auto order_depth = depth_ - reversed_depth;

        auto [begin, end] = node_ranges_in_depth(order_depth);
        size_t ret = 0;
        for (size_t i = begin; i < (size_t) end; i++)
        {
            if (!has_concurrent_alloc_at(i, true /* include me */))
            {
                ret++;
            }
        }
        return ret;
    }

    int max_order() const
    {
        return depth_;
    }

    constexpr size_t meta_bytes() const
    {
        // each node occupies 1 bit
        return buddy_.meta_bytes();
    }

    void read_meta()
    {
        dsm_->prepare_read(
            meta_rdma_buf_.buffer, meta_addr_, meta_bytes(), false, ctx_);
        rm_.read(meta_bytes());
        dsm_->commit(ctx_);
    }

    GlobalAddress page_id_to_raddr(int page_id) const
    {
        DCHECK_LT(page_id, page_nr());
        auto ret = page_addr_ + page_id * page_size_;
        return ret;
    }
    int node_id_to_page_id(int node_id) const
    {
        auto node_depth = depth(node_id);
        auto [begin, _] = node_ranges_in_depth(node_depth);
        auto node_offset = node_id - begin;
        auto page_nr = page_nr_at(node_id);
        return node_offset * page_nr;
    }

    void report() const
    {
        LOG(INFO) << "[buddy] management: " << am_ << ", IO: " << rm_;
    }
    void metric_reset()
    {
        am_.reset();
        rm_.reset();
    }

    int raddr_to_page_id(GlobalAddress raddr) const
    {
        auto offset = raddr.offset;
        DCHECK(contains(raddr));
        auto ret = (offset - page_addr_.offset) / page_size_;
        return ret;
    }

private:
    DSM::pointer dsm_;
    GlobalAddress meta_addr_;
    GlobalAddress page_addr_;
    size_t page_size_;
    std::shared_ptr<PTL> ptl_;
    size_t page_nr_;
    CoroContext *ctx_;

    // metrics
    std::map<size_t, OnePassBucketMonitor<uint64_t>> lat_;

    // order => a set of begin_nid that failed
    std::map<size_t, std::unordered_set<size_t>> remembered_failed_rng_;

    Buddy buddy_;
    // below are tree info
    int depth_;
    int node_nr_;

    // below are RDMA-related info
    Buffer meta_rdma_buf_;
    Buffer cas_rdma_buf_;

    // metrics
    size_t failed_by_conflict_{0};
    RemoteMetrics rm_;
    AllocMetrics am_;

    uint64_t trace_;

    GlobalAddress get_free_pages_bounded_random(unsigned long order)
    {
        if (unlikely(order > max_order()))
        {
            DLOG(FATAL) << "[buddy] too large to allocate: "
                        << PRE(order, max_order());
            return GlobalAddress::Null();
        }
        LOG_IF(INFO, kReport) << "[buddy] get_free_pages(" << order << ")"
                              << " " << (void *) trace_;
        auto reversed_depth = order;
        auto order_depth = depth_ - reversed_depth;

        auto [begin, end] = node_ranges_in_depth(order_depth);

        auto select_begin = begin;
        auto rng_size = 1;
        auto select_end = begin + rng_size;

        DCHECK_LE(select_end, end) << "Range overflowed";

        // [select_begin, select_end)
        while (true)
        {
            size_t cur_rng_size = select_end - select_begin;

            auto ret = GlobalAddress::Null();
            if (remembered_failed_rng_[order].count(select_begin))
            {
                // I know this is failed
                // skip
            }
            else
            {
                ret = do_get_free_pages_in(
                    select_begin, select_end, cur_rng_size /* try nr */);
            }

            if (!ret.is_null())
            {
                return ret;
            }

            if (Config::ins().buddy_remember_failure())
            {
                remembered_failed_rng_[order].insert(select_begin);
            }

            rng_size *= 2;

            select_begin = select_end;
            select_end = select_begin + rng_size;
            if (select_begin >= end)
            {
                break;
            }
            if (select_end > end)
            {
                select_end = end;
            }
        }

        return GlobalAddress::Null();
    }

    /**
     * do_get_free_pages_in gets free pages in range [begin_nid, end_nid)
     */
    GlobalAddress do_get_free_pages_in(size_t begin_nid,
                                       size_t end_nid,
                                       size_t try_nr)
    {
        // LOG(INFO) << "do_get_free_pages_in [" << begin_nid << ", " << end_nid
        //           << ") sz: " << (end_nid - begin_nid) << ", try " << try_nr;

        uint64_t start_id = fast_pseudo_rand_int(begin_nid, end_nid - 1);

        for (size_t i = 0; i < try_nr; ++i)
        {
            auto size = end_nid - begin_nid;
            auto offset = (i + start_id) % size;
            auto cur_id = begin_nid + offset;

            auto raddr = do_get_free_pages_at(cur_id);
            if (!raddr.is_null())
            {
                return raddr;
            }
        }
        return GlobalAddress::Null();
    }

    GlobalAddress do_get_free_pages_at(size_t node_id)
    {
        // pre-check
        if (has_concurrent_alloc_at(node_id, true /* include me */))
        {
            return GlobalAddress::Null();
        }

        return try_do_get_free_page_at(node_id);
    }

    GlobalAddress get_free_pages_random(unsigned long order)
    {
        LOG_IF(INFO, kReport) << "[buddy] get_free_pages(" << order << ")"
                              << " " << (void *) trace_;
        auto reversed_depth = order;
        auto order_depth = depth_ - reversed_depth;

        auto [begin, end] = node_ranges_in_depth(order_depth);
        auto size = end - begin;

        // fast path: randomly select position
        // and poll for (size/2) times
        auto try_limits = size / 2;
        // >= 8
        try_limits = std::max(try_limits, 2);
        for (int i = 0; i < size; i++)
        {
            auto select_id = fast_pseudo_rand_int(begin, end - 1);
            DCHECK_GE(select_id, begin);
            DCHECK_LT(select_id, end);

            auto ret = do_get_free_pages_at(select_id);
            if (!ret.is_null())
            {
                return ret;
            }
        }
        return GlobalAddress::Null();
    }

    // In the slow path, we do a full scan
    GlobalAddress get_free_pages_seq(unsigned long order)
    {
        auto reversed_depth = order;
        auto order_depth = depth_ - reversed_depth;

        auto [begin, end] = node_ranges_in_depth(order_depth);
        for (size_t select_id = begin; select_id < (size_t) end; ++select_id)
        {
            DCHECK_GE(select_id, begin);
            DCHECK_LT(select_id, end);
            auto ret = do_get_free_pages_at(select_id);
            if (!ret.is_null())
            {
                return ret;
            }
        }
        return GlobalAddress::Null();
    }

    /**
     * try_get_free_page_at tries to get a series of continous pages at node id
     * `id`.
     * @param id the node id
     *
     * CONTRACT:
     * This function ensures concurrent correctness.
     */
    GlobalAddress try_do_get_free_page_at(int id)
    {
        util::BitsViewMut meta_bits(meta_rdma_buf_.buffer, meta_bytes());

        write_ptl(id);

        // Note we do CAS in unit of `sizeof(uint64_t)`
        // TODO: we can compare *a little more* here
        // to fore-see the status of the subtree
        auto [meta_block_id, meta_bit_offset] = node_id_to_meta_position(id);
        auto bit_offset =
            meta_block_id * sizeof(uint64_t) * 8 + meta_bit_offset;
        DCHECK_EQ(bit_offset, node_id_to_bit_offset(id));
        auto meta_gaddr = meta_addr_ + meta_block_id * sizeof(uint64_t);
        uint64_t compare =
            *((uint64_t *) meta_rdma_buf_.buffer + meta_block_id);
        uint64_t mask = 0;
        util::BitsViewMut(&mask, sizeof(mask)).set(meta_bit_offset);
        DCHECK_EQ(util::count_ones(&mask, sizeof(mask)), 1);
        uint64_t swap = compare;
        util::BitsViewMut(&swap, sizeof(swap)).set(meta_bit_offset);
        DCHECK_NE(compare, swap) << "** expect this CAS does something";
        DCHECK_EQ(compare & mask, 0)
            << "** expect this slot empty before alloc";
        DCHECK_NE(swap & mask, 0)
            << "** expect this slot allocated after alloc";

        dsm_->prepare_cas(meta_gaddr,
                          sizeof(uint64_t),
                          compare,
                          mask,
                          swap,
                          mask,
                          cas_rdma_buf_.buffer,
                          false,
                          ctx_);

        // Attach to which a read to detect concurrency
        // Pre-read descendants
        if (avis::Config::ins().buddy_use_postorder())
        {
            // Pre-read contineous descendants
            auto [bit_off, bit_len] = get_descendant_bit_position(id);
            auto byte_off = bit_off / 8;

            // NOTE: must additionally add more one
            // because of complex bit calculation
            // without this, the algorithm is wrong.
            auto byte_nr = (bit_len + 7) / 8 + 1;
            auto desc_raddr = meta_addr_ + byte_off;
            dsm_->prepare_read(meta_rdma_buf_.buffer + byte_off,
                               desc_raddr,
                               byte_nr,
                               false,
                               ctx_);
            LOG_IF(INFO, kReport && kEnableHistory)
                << "[buddy] reading meta for decendants: byte rng [" << byte_off
                << ", " << byte_off + byte_nr << "), which is bits rng ["
                << byte_off * 8 << ", " << (byte_off + byte_nr) * 8 << ") "
                << (void *) trace_;
            rm_.read(byte_nr);
            // LOG(INFO) << "One read of " << util::pre_byte(byte_nr)
            //           << " for descendants";
        }
        else
        {
            // Pre-read scattered descendants
            size_t ongoing = 0;
            constexpr static size_t kMaxOngoing = 8;

            constexpr size_t unit_size = 8;
            auto descendants = get_descendant_node_ids(id);
            std::unordered_set<int> unit_ids;
            unit_ids.reserve(depth());
            for (int node_id : descendants)
            {
                if (node_id == id)
                {
                    continue;
                }
                auto bit_id = node_id_to_bit_offset(node_id);
                auto unit_id = bit_id / 8 / unit_size;
                unit_ids.insert(unit_id);
            }
            // I think we need to read self.
            {
                auto bit_id = node_id_to_bit_offset(id);
                auto unit_id = bit_id / 8 / unit_size;
                unit_ids.insert(unit_id);
            }

            for (auto unit_id : unit_ids)
            {
                auto offset = unit_id * 8;
                char *rdma_buf = meta_rdma_buf_.buffer + offset;
                auto raddr = meta_addr_ + offset;
                dsm_->prepare_read(rdma_buf, raddr, unit_size, false, ctx_);
                rm_.read(unit_size);
                ongoing++;
                if (ongoing >= kMaxOngoing)
                {
                    dsm_->commit(ctx_);
                    ongoing = 0;
                }
            }
            // LOG(INFO) << "Read " << unit_ids.size() << " for descendants";
        }

        // Pre-read ancestors
        constexpr size_t unit_size = 8;
        auto ancestors = get_ancestor_node_ids(id);
        std::unordered_set<int> unit_ids;
        unit_ids.reserve(depth());
        for (int node_id : ancestors)
        {
            if (node_id == id)
            {
                continue;
            }
            auto bit_id = node_id_to_bit_offset(node_id);
            auto unit_id = bit_id / 8 / unit_size;
            unit_ids.insert(unit_id);
        }
        for (auto unit_id : unit_ids)
        {
            auto offset = unit_id * 8;
            char *rdma_buf = meta_rdma_buf_.buffer + offset;
            auto raddr = meta_addr_ + offset;
            dsm_->prepare_read(rdma_buf, raddr, unit_size, false, ctx_);
            rm_.read(unit_size);
        }
        // LOG(INFO) << "Read " << unit_ids.size() << " for ancestors";
        dsm_->commit(ctx_);

        // Check whether we succeed
        // - CAS succeeded?
        uint64_t old_value = *(uint64_t *) cas_rdma_buf_.buffer;
        if ((old_value & mask) != (compare & mask))
        {
            rm_.cas(8, false);
            // CAS failed.
            LOG_IF(INFO, kReport)
                << "[buddy] FAILED CAS failed: " << std::endl
                << "old_value: "
                << util::BitsView(&old_value, sizeof(old_value))
                << "compare: " << util::BitsView(&compare, sizeof(compare))
                << "mask: " << util::BitsView(&mask, sizeof(mask)) << " "
                << (void *) trace_;

            failed_by_conflict_++;
            // NOTE: no need to revoke anything here:
            // your CAS did not succeeded
            return GlobalAddress::Null();
        }
        else
        {
            // CAS okay.
            // Maintain the buffer
            rm_.cas(8, true);
            LOG_IF(INFO, kReport)
                << "[buddy] CAS succeeded at bit " << bit_offset << " (nid "
                << id << ") " << (void *) trace_;
            meta_bits.set(bit_offset);
        }
        // - Concurrent allocation?
        if (unlikely(has_concurrent_alloc_at(id, false /* include me */)))
        {
            failed_by_conflict_++;
            revoke_get_free_pages_at(id);
            return GlobalAddress::Null();
        }

        return node_id_to_raddr(id);
    }

    size_t bit_offset_to_node_id(size_t offset) const
    {
        return buddy_.bit_offset_to_node_id(offset);
    }

    /**
     * revoke_get_free_pages_at maintains the consistency of buddy tree
     * under allocation failure by resetting the id to 0.
     * Otherwise, the memory leaked.
     */
    void revoke_get_free_pages_at(int id)
    {
        LOG_IF(INFO, kReport)
            << "[buddy] revoke at page " << id << " " << (void *) trace_;
        do_free_page_at(id);
    }

    bool do_get_allocated(std::vector<PageRun> &alloc,
                          std::vector<PageRun> &error,
                          int id) const
    {
        auto left = left_son(id);
        auto right = right_son(id);
        bool left_has_allocated = false;
        bool right_has_allocated = false;
        auto sz = page_nr_at(id);
        if (left)
        {
            DCHECK_NE(*left, id);
            left_has_allocated = do_get_allocated(alloc, error, *left);
        }
        if (right)
        {
            DCHECK_NE(*right, id);
            right_has_allocated = do_get_allocated(alloc, error, *right);
        }
        auto bit_id = node_id_to_bit_offset(id);
        bool has_allocated =
            util::BitsView(meta_rdma_buf_.buffer, meta_bytes()).is_set(bit_id);
        if (unlikely(left_has_allocated && has_allocated))
        {
            auto page_id = node_id_to_page_id(id);
            error.push_back({page_id, page_id + sz});
        }
        if (unlikely(right_has_allocated && has_allocated))
        {
            auto page_id = node_id_to_page_id(id);
            error.push_back({page_id, page_id + sz});
        }
        if (has_allocated)
        {
            auto page_id = node_id_to_page_id(id);
            LOG_IF(INFO, kReport)
                << "[buddy] page at " << page_id << " page_nr " << sz
                << " is allocated because bit " << bit_id
                << " is set. translated from node_id " << id << " "
                << (void *) trace_;
            alloc.push_back({page_id, page_id + sz});
        }
        return left_has_allocated || right_has_allocated || has_allocated;
    }

    GlobalAddress node_id_to_raddr(size_t node_id)
    {
        auto page_id = node_id_to_page_id(node_id);
        return page_id_to_raddr(page_id);
    }

    void write_ptl(int id)
    {
        if (ptl_)
        {
            // TODO: buddy id
            ptl_->record_buddy_to_bitmap(0 /* buddy id*/, id);
        }
    }

    int page_nr_at(int id) const
    {
        auto reverse_depth = depth_ - depth(id);
        return 1ull << reverse_depth;
    }

    /**
     * do_free_page_at frees the page at the id
     */
    void do_free_page_at(int id)
    {
        // Since revoke is like releasing lock,
        // we use FAA to do that for higher performance.
        // Note we do FAA in unit of `sizeof(uint64_t)`
        auto [meta_block_id, meta_bit_offset] = node_id_to_meta_position(id);
        auto bit_offset =
            meta_block_id * sizeof(uint64_t) * 8 + meta_bit_offset;

        DCHECK_EQ(bit_offset, node_id_to_bit_offset(id));
        auto meta_gaddr = meta_addr_ + meta_block_id * sizeof(uint64_t);
        uint64_t add_val = 0;
        util::BitsViewMut add_val_bs(&add_val, sizeof(add_val));
        add_val_bs.set(meta_bit_offset);
        uint64_t boundary = 0xffffffffffffffff;
        LOG_IF(INFO, kReport)
            << "[buddy] FAA at bit " << bit_offset << " (block "
            << meta_block_id << ", off " << meta_bit_offset << ") to free page "
            << id << " " << (void *) trace_;
        dsm_->prepare_faa(meta_gaddr,
                          sizeof(uint64_t),
                          add_val,
                          boundary,
                          cas_rdma_buf_.buffer,
                          false,
                          ctx_);
        rm_.faa(sizeof(uint64_t));
        dsm_->commit(ctx_);

        if constexpr (debug())
        {
            util::BitsView cas_view(cas_rdma_buf_.buffer, cas_rdma_buf_.size);
            CHECK(cas_view.is_set(meta_bit_offset))
                << "** [buddy] Possibly meta corruption: trying to free "
                << PRE(id) << " but it is not allocated: " << std::endl
                << cas_view;
        }

        util::BitsViewMut meta_bits(meta_rdma_buf_.buffer, meta_bytes());
        meta_bits.reset(bit_offset);
    }

    std::pair<int, int> node_id_to_meta_position(int node_id) const
    {
        auto bit_id = node_id_to_bit_offset(node_id);
        auto block = sizeof(uint64_t) * 8;
        auto block_id = bit_id / block;
        auto bit_offset = bit_id % block;
        return {block_id, bit_offset};
    }

    size_t cal_node_id(GlobalAddress addr, unsigned int order)
    {
        auto page_id = raddr_to_page_id(addr);
        auto reversed_depth = order;
        auto order_depth = depth_ - reversed_depth;
        auto [begin, end] = node_ranges_in_depth(order_depth);
        auto node_id = begin + page_id / (1ull << reversed_depth);
        return node_id;
    }

    size_t node_id_to_bit_offset(int id) const
    {
        return buddy_.node_id_to_bit_offset(id);
    }
    /**
     * has_concurrent_alloc_at returns whether concurrent allocated detected at
     * page run.
     * Only detect *ancestor* conflicts.
     */
    bool has_concurrent_alloc_at(int id, bool include_me) const
    {
        util::BitsView meta_bits(meta_rdma_buf_.buffer, meta_bytes());

        // Is id already allocated?
        if (include_me)
        {
            auto me_bit = node_id_to_bit_offset(id);
            CHECK_LE(me_bit / 8, meta_rdma_buf_.size);
            if (meta_bits.is_set(me_bit))
            {
                // LOG_IF(INFO, kReport)
                //     << "[buddy] detect concurrent alloc at page " << id << "
                //     " << (void*) trace_;
                return true;
            }
        }

        // Are we inside a larger allocated page?
        auto path_to_root = get_ancestor_node_ids(id);

        for (int node_id : path_to_root)
        {
            DCHECK_NE(node_id, id)
                << "get_ancestor_ids API should not return self";
            auto bit_offset = node_id_to_bit_offset(node_id);
            if (meta_bits.is_set(bit_offset))
            {
                // LOG_IF(INFO, kReport)
                //     << "[buddy] page run " << id
                //     << " NOT available: part of allocated larger page run "
                //     << node_id << " " << (void *) trace_;
                return true;
            }
        }
        LOG_IF(INFO, kReport && kEnableHistory)
            << "[buddy] validation: no conflict with ancestor: node_ids: "
            << PRE(path_to_root) << " " << (void *) trace_;

        // Are any sub-pages allocated?
        if (avis::Config::ins().buddy_use_postorder())
        {
            auto [begin_bit, bit_nr] = get_descendant_bit_position(id);
            for (int pos = begin_bit; pos < begin_bit + (int) bit_nr; pos++)
            {
                if (meta_bits.is_set(pos))
                {
                    auto conflict_nid = bit_offset_to_node_id(pos);
                    LOG_IF(INFO, kReport)
                        << "[buddy] page run at node_id: " << id
                        << " NOT available: sub page run at bit " << pos
                        << " (node_id: " << conflict_nid << ") allocated. "
                        << (void *) trace_;
                    return true;
                }
            }
            LOG_IF(INFO, kReport && kEnableHistory)
                << "[buddy] validation: no conflict with descendants: "
                   "checking bit ranges: ["
                << begin_bit << ", " << begin_bit + bit_nr << ")"
                << " " << (void *) trace_;
        }
        else
        {
            auto descendants = get_descendant_node_ids(id);
            for (auto node_id : descendants)
            {
                DCHECK_NE(node_id, id)
                    << "get_descendant_ids API should not return self";
                auto bit_offset = node_id_to_bit_offset(node_id);
                DCHECK_EQ(bit_offset, node_id)
                    << "** With buddy_use_postorder OFF, they should be the "
                       "same";
                if (meta_bits.is_set(bit_offset))
                {
                    return true;
                }
            }
            LOG_IF(INFO, kReport && kEnableHistory)
                << "[buddy] validation: no conflict with descendants: "
                   "checking bit bit nr: "
                << descendants.size() << " " << (void *) trace_;
        }
        return false;
    }
    constexpr bool is_root(int i) const
    {
        return i == 0;
    }
    constexpr bool is_leaf(int i) const
    {
        return i >= node_nr_ / 2;
    }
    constexpr bool is_node_valid(int i) const
    {
        return i >= 0 && i < node_nr_;
    }
    std::optional<int> parent(int i) const
    {
        return buddy_.parent(i);
    }
    std::optional<int> left_son(int i) const
    {
        return buddy_.left_son(i);
    }
    std::optional<int> right_son(int i) const
    {
        return buddy_.right_son(i);
    }
    int depth(int i) const
    {
        // TODO: could it be faster?
        return buddy_.depth(i);
    }
    size_t node_managed_page_nr(int i) const
    {
        auto reverse_depth = depth_ - depth(i);
        auto page_nr = 1ull << reverse_depth;
        return page_size_ * page_nr;
    }
    // return a list of node (id) that in the certain depth
    // e.g.,
    // depth_node_ranges(0) => [0, 1)
    // depth_node_ranges(1) => [1, 3)
    // depth_node_ranges(2) => [3, 7)
    // depth_node_ranges(3) => [7, 15)
    std::pair<int, int> node_ranges_in_depth(int depth) const
    {
        return buddy_.node_ranges_in_depth(depth);
    }
    std::vector<int> get_ancestor_node_ids(int id) const
    {
        return buddy_.get_ancestor_node_ids(id);
    }
    std::vector<int> get_descendant_node_ids(int id) const
    {
        return buddy_.get_descendant_node_ids(id);
    }
    std::pair<int, size_t> get_descendant_bit_position(int id) const
    {
        return buddy_.get_descendant_bit_position(id);
    }
};

inline std::ostream &operator<<(std::ostream &os, const BuddyAllocator &b)
{
    os << "{Buddy pages: " << b.page_nr() << ", depth: " << b.depth() << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const BuddyAllocator::Dump &d)
{
    os << "{" << d.allocated_page_nr_ << " / " << d.total_page_nr() << " ("
       << util::pre_byte(d.allocated_bytes()) << " / "
       << util::pre_byte(d.total_bytes()) << ")} page_size " << d.page_size();
    return os;
}

inline std::ostream &operator<<(std::ostream &os,
                                const BuddyAllocator::FragDump &d)
{
    os << "{Frag: " << util::pre(d.frag) << ". Frag-index: " << d.index << "}";
    return os;
}

}  // namespace avis