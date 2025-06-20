#include <algorithm>
#include <random>

#include "DSM.h"
#include "Timer.h"
#include "gflags/gflags.h"
#include "util/gflags_def.h"
#include "util/monitor.h"

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    VLOG(1) << "It is 1 vlog";
    VLOG(2) << "It is 2 vlog";
    VLOG(3) << "It is 3 vlog";
    LOG(INFO) << "Support color ? " << getenv("TERM");

    rdmaQueryDevice();

    DSMConfig config;

    auto dsm = DSM::getInstance(config);

    dsm->registerThread();
    auto server_nid = ::config::get_server_nids().front();

    // let client spining
    auto nid = dsm->getMyNodeID();
    if (::config::is_client(nid))
    {
        dsm->keeper_barrier("sync", 100ms);
        dsm->reconnectThreadToDir(server_nid, 0);
    }
    else
    {
        dsm->reinitializeDir(0);
        dsm->keeper_barrier("sync", 100ms);
    }

    LOG(INFO) << "finished. ctrl+C to quit.";
}