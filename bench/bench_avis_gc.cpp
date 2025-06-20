#include "Common.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "avis/avis.h"
#include "bench/DataFrame.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Rand.h"
#include "util/bits.h"
#include "util/gflags_def.h"

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    DSMConfig config;
    config.worker_nr = 0;
    auto dsm = DSM::getInstance(config);
    dsm->registerThread();

    auto server_nid = dsm->getClusterSize() - 1;

    auto ptl = std::make_shared<avis::PTL>(dsm, server_nid, nullptr);
    auto gc = std::make_shared<avis::GC>(dsm, ptl, 1024);

    for (size_t leak_size : {64, 256, 512, 1024})
    {
        auto gc = std::make_shared<avis::GC>(dsm, ptl, leak_size);
        gc->recover(8 /* parallel */);
    }

    LOG(INFO) << "PASS.";
}