#pragma once
#include <cinttypes>
#include <iostream>

#include "util/History.h"
#include "util/Pre.h"

namespace avis
{
enum State
{
    kScatter,
    kAllocated,
    kOk,
};

inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] ::avis::State e)
{
    switch (e)
    {
    case ::avis::kScatter:
        os << "kScatter";
        break;
    case ::avis::kAllocated:
        os << "kAllocated";
        break;
    case ::avis::kOk:
        os << "kOk";
        break;
    default:
        os << "Unknown ::avis::debug::State(" << (int) e << ")";
    }
    return os;
}

struct Record
{
    uint64_t block_id;
    uint64_t slot_id;
    uint64_t page_id;
    State state;
    // TODO: we don't have per-coro history yet.
    // So encode coid here for now.
    uint8_t cid;
};

inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::avis::Record &v)
{
    os << "{Record ";
    os << "block_id: " << util::pre(v.block_id);
    os << ", slot_id: " << util::pre(v.slot_id);
    os << ", page_id: " << util::pre(v.page_id);
    os << ", state: " << util::pre(v.state);
    os << ", cid: " << util::pre((int) v.cid);
    os << "}";
    return os;
}

}  // namespace avis