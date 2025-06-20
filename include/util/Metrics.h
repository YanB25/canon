#pragma once
#include <cstdint>

#include "Common.h"
#include "GlobalAddress.h"
#include "PerformanceReporter.h"
#include "sherman/TreeConfig.h"
#include "util/Pre.h"

namespace util
{
enum class OpType
{
    kRead,
    kDMRead,
    kWrite,
    kDMWrite,
    kCas,
    kDMCas,
    kFAA,
    kDMFAA,
    kMax,  // must be the last one
};
inline std::ostream &operator<<(std::ostream &os, OpType op)
{
    switch (op)
    {
    case OpType::kRead:
        os << "read";
        break;
    case OpType::kDMRead:
        os << "dm-read";
        break;
    case OpType::kWrite:
        os << "write";
        break;
    case OpType::kDMWrite:
        os << "dm-write";
        break;
    case OpType::kCas:
        os << "cas";
        break;
    case OpType::kDMCas:
        os << "dm-cas";
        break;
    case OpType::kFAA:
        os << "faa";
        break;
    default:
        os << "unknown(" << (int) op << ")";
    }
    return os;
}

class MetricCollector
{
public:
    constexpr static size_t kLockPerMachine = define::kNumOfLock;
    constexpr static size_t kLockNr = MAX_MACHINE * kLockPerMachine;

    MetricCollector()
    {
        reset();
    }

    void collect(GlobalAddress gaddr, size_t size, OpType op)
    {
        std::ignore = op;
        if constexpr (::config::kMonitorAccessDistribution)
        {
            m_[(int) op].node_[gaddr.nodeID]++;
            m_[(int) op].node_size_[gaddr.nodeID] += size;
            if (op == OpType::kDMCas)
            {
                auto local_lock_id = gaddr.offset / 8;
                CHECK_LT(local_lock_id, kLockPerMachine);
                auto g_lock_id = gaddr.nodeID * kLockPerMachine + local_lock_id;
                CHECK_LT(g_lock_id, kLockNr);
                g_lock_m_.collect(g_lock_id);
                lock_ms_[gaddr.nodeID].collect(local_lock_id);
            }
        }
    }
    void reset()
    {
        m_.clear();
        g_lock_m_.reset(0, kLockNr, 1);
        lock_ms_.clear();
        for (size_t i = 0; i < MAX_MACHINE; ++i)
        {
            lock_ms_.emplace_back(0, kLockNr, 1);
        }
    }
    friend std::ostream &operator<<(std::ostream &, const MetricCollector &);

private:
    struct AccessMetric
    {
        std::array<uint64_t, MAX_MACHINE> node_{};
        std::array<uint64_t, MAX_MACHINE> node_size_{};
    };
    // op_type to AccessMetric
    std::unordered_map<size_t, AccessMetric> m_;
    std::vector<OnePassBucketMonitor<uint64_t>> lock_ms_;
    OnePassBucketMonitor<uint64_t> g_lock_m_{0, kLockNr, 1};
};

inline std::ostream &operator<<(std::ostream &os, const MetricCollector &c)
{
    os << "Metrics:" << std::endl;
    for (int i = 0; i < (int) OpType::kMax; ++i)
    {
        auto it = c.m_.find(i);
        if (it != c.m_.end())
        {
            os << (OpType) i << "(node): " << util::pre(it->second.node_)
               << std::endl;
            os << (OpType) i << "(size): " << util::pre(it->second.node_size_)
               << std::endl;
        }
    }
    auto bucket = c.g_lock_m_.buckets();
    std::sort(bucket.begin(), bucket.end());
    auto not_zero_nr =
        std::count_if(bucket.begin(),
                      bucket.end(),
                      [](uint64_t val) -> bool { return val != 0; });
    auto max_val = bucket.empty() ? 0 : bucket.back();
    auto min_val = bucket.empty() ? 0 : bucket.front();
    auto within_max_tenth_nr = std::count_if(
        bucket.begin(),
        bucket.end(),
        [target = 1.0 * max_val * 0.9](uint64_t val) { return val >= target; });
    os << "LOCK: " << util::pre(bucket, 20) << std::endl;
    os << "LOCK: max: " << max_val << ", min: " << min_val
       << ", not zero nr: " << not_zero_nr
       << ", in 0.9 of max nr: " << within_max_tenth_nr << std::endl;
    CHECK_EQ(c.g_lock_m_.overflow_nr(), 0) << "** overflowed. ";
    CHECK_EQ(c.g_lock_m_.underflow_nr(), 0) << "** overflowed. ";

    for (size_t i = 0; i < MAX_MACHINE; ++i)
    {
        auto bucket2 = c.lock_ms_[i].buckets();
        std::sort(bucket2.begin(), bucket2.end());
        os << "LOCK[" << i << "] " << util::pre(bucket2, 20) << std::endl;
    }
    return os;
}

}  // namespace util