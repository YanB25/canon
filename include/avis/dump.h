#pragma once

#include "DSM.h"
#include "avis/bitmap.h"
#include "avis/buddy.h"
#include "avis/provider.h"
#include "util/PreUtil.h"

namespace avis
{
class Dumper
{
public:
    Dumper(DSM::pointer dsm, const std::vector<BuddyProvider> &providers)
        : dsm_(dsm), providers_(providers)
    {
        for (size_t buddy_id = 0; buddy_id < providers_.size(); ++buddy_id)
        {
            const auto &p = providers_[buddy_id];
            auto &ctx = dumps_[buddy_id];

            buddys_.emplace_back(std::make_shared<BuddyAllocator>(dsm_,
                                                                  p.meta_raddr,
                                                                  p.meta_size,
                                                                  p.buf_raddr,
                                                                  p.buf_size,
                                                                  p.page_size,
                                                                  nullptr,
                                                                  nullptr));

            auto &buddy = buddys_.back();
            ctx.buddy_dump = buddy->dump();
            ctx.buddy_frag = buddy->dump_fragmentation();
            auto [alloced, _] = buddy->get_allocated();
            for (const auto &[begin, end] : alloced)
            {
                auto raddr = buddy->page_id_to_raddr(begin);

                auto rdma_buf = dsm_->get_rdma_buffer(sizeof(BitmapHeader));
                dsm_->prepare_read(rdma_buf.buffer,
                                   raddr,
                                   sizeof(BitmapHeader),
                                   false,
                                   nullptr);
                dsm_->commit(nullptr);
                BitmapHeader &header = *(BitmapHeader *) rdma_buf.buffer;
                if (header.valid())
                {
                    bitmaps_[buddy_id].emplace_back(
                        std::make_shared<avis::BitmapSlab>(
                            dsm_,
                            raddr,
                            buddy->page_size() * (end - begin),
                            header.obj_size,
                            nullptr,
                            nullptr));
                    auto &bitmap = bitmaps_[buddy_id].back();
                    auto dump = bitmap->dump();
                    ctx.bitmap_dumps.emplace_back(dump);
                }
                else
                {
                    ctx.get_free_page_nr++;
                    ctx.get_free_page_page_nr += (end - begin);
                }
                dsm_->put_rdma_buffer(std::move(rdma_buf));
            }
        }
    }

    std::pair<size_t, size_t> bitmap_utilizations() const
    {
        size_t total = 0;
        size_t used = 0;
        for (const auto &[buddy_id, ctx] : dumps_)
        {
            for (const auto &bitmap_dump : ctx.bitmap_dumps)
            {
                used += bitmap_dump.allocated_size();
                total += bitmap_dump.total_size();
            }
        }
        return {used, total};
    }

    void report(bool verbose = true)
    {
        std::map<size_t, size_t> total_waste;  // size class => waste bytes
        std::map<size_t, size_t> total_nr;  // size class => number of bitmaps
        std::map<size_t, size_t>
            partial_nr;  // size class => number of partial bitmaps

        LOG(INFO) << "============ report ===========";
        for (size_t id = 0; id < dumps_.size(); id++)
        {
            const auto &ctx = dumps_[id];

            // This map is used to make size ordered
            struct SizedSummary
            {
                std::vector<BitmapSlab::Dump> partial_dumps;
                size_t all_allocated_nr{};
                size_t not_allocated_nr{};
            };
            std::map<size_t, SizedSummary> ordered_dumps;
            for (const auto &d : ctx.bitmap_dumps)
            {
                if (d.all_allocated())
                {
                    ordered_dumps[d.object_size_].all_allocated_nr++;
                }
                else if (d.not_allocated())
                {
                    ordered_dumps[d.object_size_].not_allocated_nr++;
                }
                else
                {
                    ordered_dumps[d.object_size_].partial_dumps.emplace_back(d);
                }
            }

            LOG(INFO) << "buddy (" << id << "): " << ctx.buddy_dump
                      << ", bitmap_nr: " << ctx.bitmap_dumps.size() << " with "
                      << ordered_dumps.size() << " size classes" << std::endl;
            // LOG(INFO) << util::pre(ctx.buddy_frag) << std::endl;
            LOG(INFO) << "Frag-index: " << ctx.buddy_frag.index;
            std::vector<size_t> nr_per_order;
            for (const auto &[order, nr] : ctx.buddy_frag.frag)
            {
                nr_per_order.push_back(nr);
            }
            LOG(INFO) << "number of pages / order: " << util::pre(nr_per_order);

            if (verbose)
            {
                for (const auto &[size, sized_summary] : ordered_dumps)
                {
                    LOG(INFO)
                        << " * size: " << size
                        << ", full_alloced: " << sized_summary.all_allocated_nr
                        << ", not_alloced: " << sized_summary.not_allocated_nr
                        << ", partial: " << sized_summary.partial_dumps.size()
                        << std::endl;
                    for (const auto &d : sized_summary.partial_dumps)
                    {
                        LOG(INFO) << "    - " << d;
                        total_waste[d.object_size_] += d.available_size();
                    }
                    total_nr[size] += sized_summary.all_allocated_nr +
                                      sized_summary.not_allocated_nr +
                                      sized_summary.partial_dumps.size();
                    partial_nr[size] += sized_summary.partial_dumps.size();
                }
                if (ctx.get_free_page_nr)
                {
                    LOG(INFO)
                        << "    - "
                        << "Others: " << ctx.get_free_page_page_nr
                        << " pages in " << ctx.get_free_page_nr << " calls";
                }
            }
        }

        // if (verbose)
        // {
        //     uint64_t total_waste_byte = 0;
        //     for (const auto &[size, waste_bytes] : total_waste)
        //     {
        //         LOG(INFO) << " [" << size << "] waste " << waste_bytes << "
        //         ("
        //                   << util::pre_byte(waste_bytes) << ") from "
        //                   << partial_nr[size] << " out of " << total_nr[size]
        //                   << " bitmaps";
        //         total_waste_byte += waste_bytes;
        //     }
        //     LOG(INFO) << "[summary] waste " << total_waste_byte << " ("
        //               << util::pre_byte(total_waste_byte) << ").";
        // }
        LOG(INFO) << "============ END ===========";
    }

private:
    DSM::pointer dsm_;
    std::vector<BuddyProvider> providers_;
    std::vector<std::shared_ptr<BuddyAllocator>> buddys_;
    // buddy_id => [BitmapSlab]
    std::map<size_t, std::vector<std::shared_ptr<BitmapSlab>>> bitmaps_;

    struct Ctx
    {
        BuddyAllocator::Dump buddy_dump;
        BuddyAllocator::FragDump buddy_frag;
        double frag_index;
        std::vector<BitmapSlab::Dump> bitmap_dumps;
        size_t get_free_page_nr{};
        size_t get_free_page_page_nr{};
    };

    // buddy_id => Ctx (Buddy::Dump + [BitmapSlab::Dump])
    std::map<size_t, Ctx> dumps_;
};
}  // namespace avis