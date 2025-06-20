#include "util/Numa.h"

#include "util/gflags_dec.h"

namespace util
{
thread_local std::optional<size_t> NUMACtl::cpu_affinity_;

NUMADesc::NUMADesc()
{
    CHECK_EQ(numa_available(), 0);
    total_cores_nr_ = numa_num_task_cpus();
    total_numa_nr_ = numa_num_task_nodes();

    for (size_t i = 0; i < total_numa_nr_; ++i)
    {
        numa_own_cpus_.push_back(CHECK_NOTNULL(numa_allocate_cpumask()));
        int ret = numa_node_to_cpus(i, numa_own_cpus_.back());
        PLOG_IF(FATAL, ret != 0) << "[NUMA] failed to call numa_node_to_cpus";
    }

    for (size_t numa_id = 0; numa_id < total_numa_nr_; ++numa_id)
    {
        numa_id_map_.emplace_back();
        auto &map = numa_id_map_.back();

        size_t thread_id = 0;
        for (size_t cpu_id = 0; cpu_id < total_cores_nr_; cpu_id++)
        {
            if (node_own_cpu(numa_id, cpu_id))
            {
                map.cpu_id_to_thread_id[cpu_id] = thread_id;
                map.thread_id_to_cpu_id[thread_id] = cpu_id;
                thread_id++;
                map.cpu_nr_++;
            }
        }
    }
}

size_t NUMADesc::to_cpu_id(size_t numa_id, size_t thread_id) const
{
    DCHECK_EQ(numa_id_map_.size(), total_numa_nr_);
    CHECK_LT(numa_id, numa_id_map_.size());
    auto &map = numa_id_map_[numa_id];
    auto it = map.thread_id_to_cpu_id.find(thread_id);
    if (unlikely(it == map.thread_id_to_cpu_id.end()))
    {
        LOG(FATAL) << "[NUMA] can not convert to cpu id: not found for numa: "
                   << numa_id << ", thread_id: " << thread_id;
    }
    return it->second;
}

size_t NUMADesc::to_thread_id(size_t numa_id, size_t cpu_id) const
{
    DCHECK_EQ(numa_id_map_.size(), total_numa_nr_);
    CHECK_LT(numa_id, numa_id_map_.size());
    auto &map = numa_id_map_[numa_id];
    auto it = map.cpu_id_to_thread_id.find(cpu_id);
    if (unlikely(it == map.cpu_id_to_thread_id.end()))
    {
        LOG(FATAL) << "[NUMA] not found for numa: " << numa_id
                   << ", cpu_id: " << cpu_id;
    }
    return it->second;
}

NUMADesc::~NUMADesc()
{
    for (bitmask *bm : numa_own_cpus_)
    {
        numa_bitmask_free(bm);
    }
    numa_own_cpus_.clear();
}

NUMACtl::NUMACtl(size_t numa_affinity) : numa_affinity_(numa_affinity)
{
    set_numa_affinity(numa_affinity);
}
void NUMACtl::set_core_affinity(size_t cpu_id)
{
    LOG_IF(WARNING, cpu_affinity_.has_value())
        << "[NUMA] this thread already bind to " << cpu_affinity_.value()
        << ". rebind to " << cpu_id;
    CHECK(numa_desc_.node_own_cpu(numa_affinity_, cpu_id))
        << "[NUMA] numa_id " << numa_affinity_ << " does not own cpu "
        << cpu_id;
    bindCore(cpu_id);
    cpu_affinity_ = cpu_id;

    CoreConflictDetector::ins().record_bind_core(cpu_id);
}

void NUMACtl::bindCore(size_t cpu_id)
{
    std::ignore = cpu_id;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0)
    {
        LOG(ERROR) << "can't bind core " << cpu_id;
    }
}
void NUMACtl::set_numa_affinity(size_t numa_affinity)
{
    CHECK_LT(numa_affinity, numa_desc_.numa_nr());
    // (1)
    int ret = numa_run_on_node(numa_affinity);
    PLOG_IF(FATAL, ret != 0) << "[NUMA] failed to call numa_run_on_node_mask";
    auto *node_mask =
        CHECK_NOTNULL(numa_bitmask_alloc(numa_max_possible_node()));
    numa_bitmask_setbit(node_mask, numa_affinity);
    // (2)
    CHECK(numa_bitmask_isbitset(node_mask, numa_affinity));
    numa_set_membind(node_mask);
    numa_bitmask_free(node_mask);
    numa_affinity_ = numa_affinity;
}
std::ostream &operator<<(std::ostream &os, const bitmask &bm)
{
    os << "[";
    size_t cnt = 0;
    for (size_t i = 0; i < bm.size; ++i)
    {
        bool y = numa_bitmask_isbitset(&bm, i);
        cnt += y;
        os << (y ? 1 : 0) << ", ";
    }
    os << "] (size: " << bm.size << ", ones: " << cnt << ")";
    return os;
}

std::ostream &operator<<(std::ostream &os, const NUMADesc &n)
{
    os << "{NUMADesc: node_nr: " << n.numa_nr()
       << ", cpu_nr: " << n.total_cpu_nr() << ", " << std::endl;
    for (size_t numa_id = 0; numa_id < n.numa_nr(); ++numa_id)
    {
        os << "numa(" << numa_id << ") has " << n.node_cpu_nr(numa_id)
           << " cpus, they are " << std::endl;
        os << *n.get_bitmask(numa_id) << std::endl;
        os << "tid2cpuid(" << numa_id << ")"
           << util::pre(n.numa_id_map_[numa_id].thread_id_to_cpu_id)
           << std::endl;
    }
    os << "}";
    return os;
}

void NUMACtl::explain() const
{
    auto *mb = numa_get_membind();
    auto *rb = numa_get_run_node_mask();
    LOG(INFO) << "membind: " << *mb << ", runnode: " << *rb;
    numa_free_nodemask(rb);
    numa_free_nodemask(mb);
}

}  // namespace util