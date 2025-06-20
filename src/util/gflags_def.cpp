#include <csignal>
#include <exception>

#include "Common.h"
#include "HugePageAlloc.h"
#include "util/ProcessCleaner.h"
#include "util/gflags_dec.h"

void init_gflags(int &argc, ArgvT &argv)
{
    google::InitGoogleLogging(argv[0]);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    FLAGS_binary = argv[0];

    LOG(INFO) << "[gflags] exec_meta " << FLAGS_exec_meta
              << ", rnic: " << FLAGS_rnic
              << ", machine_nr: " << FLAGS_machine_nr
              << ", node_id: " << FLAGS_node_id
              << ", numa_id: " << FLAGS_numa_id;
    CHECK_LE(FLAGS_machine_nr, MAX_MACHINE);

    auto server_list = ::config::get_server_nids();
    auto client_list = ::config::get_client_nids();
    [[maybe_unused]] auto server_nr = server_list.size();
    [[maybe_unused]] auto client_nr = client_list.size();
    // CHECK_EQ(FLAGS_machine_nr, client_nr + server_nr)
    // LOG_IF(WARNING, FLAGS_machine_nr != client_nr + server_nr)
    //     << "** Config mismatch. machine_nr: " << FLAGS_machine_nr
    //     << ", client_nr: " << client_nr << ", server_nr: " << server_nr;

    size_t rnic_idx = FLAGS_rnic.back() - '0';
    LOG_IF(WARNING, rnic_idx != FLAGS_numa_id)
        << "** Possible open a cross-numa RNIC. rnic_idx: " << rnic_idx
        << " from " << FLAGS_rnic << " in numa: " << FLAGS_numa_id;

    if constexpr (::config::kEnableAbrtHandler)
    {
        register_signal_handlers();
    }
}
