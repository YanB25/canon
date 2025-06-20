#pragma once

#include "./config.h"
#include "./provider.h"
#include "GlobalAddress.h"
#include "avis/bitmap.h"
#include "avis/debug.h"

namespace avis
{
class Locator
{
public:
    struct Location
    {
        size_t buddy_id;
        GlobalAddress bitmap_meta;
        size_t bitmap_size;
    };
    using Provider = BuddyProvider;
    Locator(const std::vector<Provider> &providers) : providers_(providers)
    {
    }

    std::optional<Location> locate(GlobalAddress addr, size_t object_size) const
    {
        for (size_t buddy_id = 0; buddy_id < providers_.size(); ++buddy_id)
        {
            const auto &p = providers_[buddy_id];
            DCHECK_EQ(p.node_id, addr.nodeID) << "node_id mismatch.";

            if (addr >= p.buf_raddr && addr < p.buf_raddr + p.buf_size)
            {
                Location ret;
                ret.buddy_id = buddy_id;

                ret.bitmap_size = Config::ins().bitmap_size(object_size);

                auto page_id = raddr_to_page_id(p, addr);
                DCHECK_EQ(ret.bitmap_size % p.page_size, 0);
                auto page_nr = ret.bitmap_size / p.page_size;
                auto [page_begin, _] = locate_page_run(page_id, page_nr);
                auto bitmap_begin = page_id_to_raddr(p, page_begin);
                ret.bitmap_meta = bitmap_begin;
                return ret;
            }
        }
        avis_debug();
        LOG(FATAL) << "Unknown how to locate addr " << addr << " with size "
                   << object_size << std::endl
                   << PRE(providers_);
        return std::nullopt;
    }

private:
    int raddr_to_page_id(const Provider &p, GlobalAddress raddr) const
    {
        auto offset = raddr.offset;
        return (offset - p.buf_raddr.offset) / p.page_size;
    }
    std::pair<int, int> locate_page_run(int page_id, size_t page_nr) const
    {
        DCHECK(util::is_power_of_two(page_nr));
        auto start = page_id - (page_id % page_nr);
        auto end = start + page_nr;
        return {start, end};
    }
    GlobalAddress page_id_to_raddr(const Provider &p, size_t ith) const
    {
        return p.buf_raddr + ith * p.page_size;
    }

    const std::vector<Provider> providers_;
};

inline std::ostream &operator<<(std::ostream &os, const Locator::Location &l)
{
    os << "{Location buddy: " << l.buddy_id << ", bitmap: " << l.bitmap_meta
       << ", size: " << util::pre_byte(l.bitmap_size) << "}";
    return os;
}

}  // namespace avis