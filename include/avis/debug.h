#pragma once
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>

#include "DSMCache.h"
#include "avis/mm.h"
#include "avis/policy/debug.h"
#include "util/CRTP.h"
#include "util/History.h"

namespace avis
{
struct HisRecord : util::Hashable<HisRecord>
{
    bool is_alloc;
    GlobalAddress raddr;
    size_t size;
    int tid;
    int cid;
    bool operator==(const HisRecord &rhs) const
    {
        return is_alloc == rhs.is_alloc && raddr == rhs.raddr &&
               size == rhs.size && tid == rhs.tid && cid == rhs.cid;
    }
};

inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::avis::HisRecord &v)
{
    os << "{Record ";
    os << "is_alloc: " << util::pre(v.is_alloc);
    os << ", raddr: " << util::pre(v.raddr);
    os << ", size: " << util::pre(v.size);
    os << ", tid: " << util::pre(v.tid);
    os << ", cid: " << util::pre(v.cid);
    os << "}";
    return os;
}

extern util::TL_History<HisRecord> history;
extern util::TL_History<HisRecord> buddy_history_;
extern util::TL_History<HisRecord> cache_history_;

inline void validate_history(const util::TL_History<HisRecord> &input)
{
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);

    auto all_his = input.all();
    auto all_his_reverse = all_his | ranges::views::reverse;

    std::unordered_set<HisRecord> allocated;
    for (const auto &item : all_his_reverse)
    {
        // validate first
        if (item->is_alloc)
        {
            for (const auto &previous : allocated)
            {
                Buffer previous_buf((char *) previous.raddr.offset,
                                    previous.size);
                Buffer cur_buf((char *) item->raddr.offset, item->size);
                if (!test_buffer_not_overlapped(previous_buf, cur_buf))
                {
                    LOG(FATAL) << "** Double allocation detected: " << std::endl
                               << "Current allocation: " << item << std::endl
                               << "Conflict with" << previous;
                }
            }
        }
        else
        {
            // it is free.
            bool found = false;
            // for (const auto &previous : allocated)
            for (auto it = allocated.begin(); it != allocated.end(); ++it)
            {
                const auto &previous = *it;
                if (previous.raddr == item->raddr &&
                    previous.size == item->size)
                {
                    found = true;
                    CHECK_EQ(allocated.erase(*it), 1);
                    break;
                }
            }
            CHECK(found) << "** Freeing not allocated: " << PRE(item);
        }
        // maintain states
        bool ok = allocated.insert(*item).second;
        CHECK(ok);
    }
}

inline void avis_debug()
{
    validate_history(buddy_history_);
    LOG(INFO) << "PASS buddy. checked " << buddy_history_.all().size()
              << " histories.";
    validate_history(cache_history_);
    LOG(INFO) << "PASS cache. checked " << buddy_history_.all().size()
              << " histories.";
}

}  // namespace avis