#pragma once
#include "util/Pre.h"
namespace bench
{
struct ResultLatencyRecord
{
    std::optional<uint64_t> min{};
    std::optional<uint64_t> p50{};
    std::optional<uint64_t> p90{};
    std::optional<uint64_t> p99{};
    std::optional<uint64_t> p999{};
    std::optional<uint64_t> p9999{};
    std::optional<uint64_t> max{};
};

inline std::ostream &operator<<(std::ostream &os, const ResultLatencyRecord &r)
{
    if (r.p50)
    {
        os << "p50: " << util::pre_ns(*r.p50);
    }
    if (r.p90)
    {
        os << "p90: " << util::pre_ns(*r.p90);
    }
    if (r.p99)
    {
        os << "p99: " << util::pre_ns(*r.p99);
    }
    if (r.p999)
    {
        os << "p999: " << util::pre_ns(*r.p999);
    }

    return os;
}
struct ResultRecord
{
    double cluster_ops{};
    double variaty{};

    ResultLatencyRecord lat_ns{};
};

inline std::ostream &operator<<(std::ostream &os, const ResultRecord &r)
{
    os << "cluster ops: " << util::pre_ops(r.cluster_ops)
       << ", variaty: " << util::pre_pcnt(r.variaty) << ", lat: " << r.lat_ns;

    return os;
}

}  // namespace bench