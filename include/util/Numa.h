#pragma once
#include <numa.h>

#include <map>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

#include "glog/logging.h"
#include "util/Likely.h"
#include "util/Pre.h"
#include "util/gflags_dec.h"
namespace util
{
class NUMADesc
{
public:
    NUMADesc();

    size_t numa_nr() const
    {
        return total_numa_nr_;
    }
    size_t total_cpu_nr() const
    {
        return total_cores_nr_;
    }
    size_t node_cpu_nr(size_t numa_id) const
    {
        CHECK_LT(numa_id, numa_id_map_.size());
        return numa_id_map_[numa_id].cpu_nr_;
    }

    bool node_own_cpu(size_t numa_id, size_t cpu_id) const
    {
        CHECK_LT(numa_id, total_numa_nr_);
        CHECK_LT(cpu_id, total_cores_nr_);
        return node_of_cpu(cpu_id) == numa_id;
    }
    size_t node_of_cpu(size_t cpu_id) const
    {
        return numa_node_of_cpu(cpu_id);
    }

    size_t to_cpu_id(size_t numa_id, size_t thread_id) const;
    size_t to_thread_id(size_t numa_id, size_t cpu_id) const;

    const bitmask *get_bitmask(size_t numa_id) const
    {
        CHECK_LT(numa_id, total_numa_nr_);
        CHECK_EQ(total_numa_nr_, numa_own_cpus_.size());
        return numa_own_cpus_[numa_id];
    }
    friend std::ostream &operator<<(std::ostream &os, const NUMADesc &n);

    ~NUMADesc();

private:
    size_t total_cores_nr_;
    size_t total_numa_nr_;
    std::vector<bitmask *> numa_own_cpus_;

    struct IDMap
    {
        std::map<size_t, size_t> cpu_id_to_thread_id;
        std::map<size_t, size_t> thread_id_to_cpu_id;
        size_t cpu_nr_{0};
    };
    std::vector<IDMap> numa_id_map_;
};  // namespace util

std::ostream &operator<<(std::ostream &os, const bitmask &bm);
std::ostream &operator<<(std::ostream &os, const NUMADesc &n);

class CoreConflictDetector
{
public:
    static CoreConflictDetector &ins()
    {
        static CoreConflictDetector d;
        return d;
    }

    void record_bind_core(int core_id)
    {
        std::lock_guard<std::mutex> lk(mu_);
        CHECK_EQ(used_core_.count(core_id), 0)
            << "Core " << core_id << " already bind. Conflict detected.";
        used_core_.insert(core_id);
    }

private:
    CoreConflictDetector() = default;
    std::unordered_set<int> used_core_;
    std::mutex mu_;
};

class NUMACtl
{
public:
    static NUMACtl &tl_ins()
    {
        static thread_local NUMACtl ins{FLAGS_numa_id};
        return ins;
    }
    void set_core_affinity(size_t cpu_id);
    const auto &numa_desc() const
    {
        return numa_desc_;
    }
    size_t cpu_nr() const
    {
        return numa_desc_.node_cpu_nr(numa_affinity_);
    }
    bool own_cpu(size_t cpu_id) const
    {
        return numa_desc_.node_own_cpu(numa_affinity_, cpu_id);
    }
    size_t to_cpu_id(size_t thread_id) const
    {
        return numa_desc_.to_cpu_id(numa_affinity_, thread_id);
    }
    size_t to_thread_id(size_t cpu_id) const
    {
        return numa_desc_.to_thread_id(numa_affinity_, cpu_id);
    }
    const bitmask *get_bitmask() const
    {
        return numa_desc_.get_bitmask(numa_affinity_);
    }
    void set_core_affinity_by_thread_id(size_t thread_id)
    {
        auto target_core_id = to_cpu_id(thread_id);
        set_core_affinity(target_core_id);
    }
    bool try_set_core_affinity_by_thread_id(size_t thread_id)
    {
        auto target_core_id = to_cpu_id(thread_id);
        if (cpu_affinity_.has_value())
        {
            // ok, set to the same core
            if (cpu_affinity_.value() == target_core_id)
            {
                return true;
            }
            // no, override to a different core
            return false;
        }
        else
        {
            // okay, it is the first time
            set_core_affinity(target_core_id);
            return true;
        }
    }

    static void bindCore(size_t cpu_id);

    size_t get_numa_affinity() const
    {
        return numa_affinity_;
    }
    size_t get_cpu_affinity() const
    {
        CHECK(cpu_affinity_.has_value());
        return cpu_affinity_.value();
    }
    void explain() const;

    friend std::ostream &operator<<(std::ostream &os, const NUMACtl &c);

private:
    NUMACtl(size_t numa_id);
    NUMADesc numa_desc_;
    size_t numa_affinity_;
    static thread_local std::optional<size_t> cpu_affinity_;

    void set_numa_affinity(size_t numa_affinity);
};

inline std::ostream &operator<<(std::ostream &os, const NUMACtl &c)
{
    os << "{NumaCtl numa: " << c.get_numa_affinity()
       << ", core: " << util::pre(c.cpu_affinity_) << "}";

    return os;
}

}  // namespace util