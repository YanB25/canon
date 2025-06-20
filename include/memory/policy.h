#pragma once
#include "Common.h"

namespace mem
{
using flag_t = uint64_t;
enum class AllocFlag : uint8_t
{
    kForceRemote = 1 << 0,  // the call must use RPC
    kMock = 1 << 1,         // server may return any address
};

inline std::ostream &operator<<(std::ostream &os, const AllocFlag &f)
{
    auto f_ = (flag_t) f;
    bool force_rm = f_ & (flag_t) AllocFlag::kForceRemote;
    bool mock = f_ & (flag_t) AllocFlag::kMock;
    if (force_rm)
    {
        os << "kForceRemote ";
    }
    if (mock)
    {
        os << "kMock ";
    }
    return os;
}

struct Policy
{
    uint8_t flags;  // use AllocFlag
    size_t batch_size_;
};

static Policy default_policy =
    Policy{.flags = (flag_t) 0, .batch_size_ = define::kChunkSize};
}  // namespace mem