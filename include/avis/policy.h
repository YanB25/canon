#pragma once
#include <cstddef>
#include <iostream>

namespace avis
{
enum class AllocatorType
{
    kBaseline,
    kBatch,
    kRPC,
};

#include <iostream>
inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] ::avis::AllocatorType e)
{
    switch (e)
    {
    case ::avis::AllocatorType::kBaseline:
        os << "kBaseline";
        break;
    case ::avis::AllocatorType::kBatch:
        os << "kBatch";
        break;
    case ::avis::AllocatorType::kRPC:
        os << "kRPC";
        break;
    default:
        os << "Unknown ::avis::AllocatorType(" << (int) e << ")";
    }
    return os;
}
struct AllocPolicy
{
    union
    {
        struct
        {
        } baseline;
        struct
        {
        } batch;
        struct
        {
        } rpc;
    };
};
inline std::ostream &operator<<(std::ostream &os, const AllocPolicy &)
{
    os << "{AP }";
    return os;
}
struct FreePolicy
{
    union
    {
        struct
        {
        } baseline;
        struct
        {
            size_t unsync_base_limit;
        } batch;
        struct
        {
        } rpc;
    };
};

inline std::ostream &operator<<(std::ostream &os, const FreePolicy &)
{
    os << "{FP }";
    return os;
}

struct Policy
{
    AllocatorType allocator_type_{AllocatorType::kBaseline};
    AllocPolicy ap;
    FreePolicy fp{};
};

inline std::ostream &operator<<(std::ostream &os, const Policy &p)
{
    os << "{Policy " << p.allocator_type_ << " alloc: ";
    switch (p.allocator_type_)
    {
    case AllocatorType::kBaseline:
    {
        os << "{ }";
        break;
    }
    case AllocatorType::kBatch:
    {
        os << "{ }";
        break;
    }
    case AllocatorType::kRPC:
    {
        os << "{ }";
        break;
    }
    }
    os << ", free: ";
    switch (p.allocator_type_)
    {
    case AllocatorType::kBaseline:
    {
        os << "{ }";
        break;
    }
    case AllocatorType::kBatch:
    {
        const auto &d = p.fp.batch;
        os << "{unsync_base_limit: " << d.unsync_base_limit << "}";
        break;
    }
    case AllocatorType::kRPC:
    {
        os << "{ }";
        break;
    }
    }

    return os;
}

class PolicyFactory
{
public:
    Policy batch_policy(size_t unsync_base_limit)
    {
        Policy policy;
        policy.allocator_type_ = AllocatorType::kBatch;
        policy.fp.batch.unsync_base_limit = unsync_base_limit;
        return policy;
    }
    Policy rpc_policy()
    {
        Policy policy;
        policy.allocator_type_ = AllocatorType::kRPC;
        return policy;
    }
    Policy baseline_policy()
    {
        Policy policy;
        policy.allocator_type_ = AllocatorType::kBaseline;
        return policy;
    }
    Policy default_policy()
    {
        return baseline_policy();
    }

private:
};

}  // namespace avis