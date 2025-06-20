#include <numa.h>

#include <thread>

#include "DSM.h"
#include "HugePageAlloc.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Numa.h"
#include "util/gflags_def.h"

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig conf;
    CHECK_EQ(conf.numa_id, FLAGS_numa_id);
    CHECK_EQ(conf.machineNR, FLAGS_machine_nr);

    LOG(INFO) << "Hei!, meta is " << FLAGS_exec_meta << ", numa_id is "
              << FLAGS_numa_id << ", selecting " << conf.rnic;

    auto dsm = DSM::getInstance(conf);
    dsm->registerThread();
    LOG(INFO) << "[debug] node: " << dsm->get_node_id()
              << ", numa: " << dsm->get_numa_id()
              << ", thread: " << dsm->get_thread_id()
              << ", cpu: " << dsm->get_cpu_id();

    std::vector<std::thread> threads;
    for (size_t i = 1; i < dsm->hardware_concurrency() - 2; ++i)
    {
        threads.emplace_back([dsm]() {
            dsm->registerThread();
            LOG(INFO) << "[debug] node: " << dsm->get_node_id()
                      << ", numa: " << dsm->get_numa_id()
                      << ", thread: " << dsm->get_thread_id()
                      << ", cpu: " << dsm->get_cpu_id();
            while (true)
            {
            }
        });
    }

    for (auto &t : threads)
    {
        t.join();
    }
    LOG(INFO) << "Finished.";
}