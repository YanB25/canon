#include <chrono>
#include <thread>

#include "DSM.h"
#include "GlobalAddress.h"
#include "avis/avis.h"
#include "avis/buddy.h"
#include "avis/debug.h"
#include "avis/dump.h"
#include "avis/provider.h"
#include "bench/experiment.h"
#include "bench/request.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "sherman/Tree.h"
#include "util/PreUtil.h"
#include "util/Rand.h"
#include "util/gflags_dec.h"
#include "util/gflags_def.h"

using namespace patronus::hash;

using Provider = avis::BuddyProvider;

struct Spec
{
    struct CLS
    {
        std::shared_ptr<avis::BuddyAllocator> buddy_;
    };
};

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using BLS = typename Base::BLS;

    using Provider = avis::BuddyProvider;
    Experiment()
    {
        DSMConfig config;
        config.relaxed_ordering = false;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();

        auto nid = dsm_->get_node_id();
        meta_size_ = 2_MB;
        meta_ = dsm_->alloc_from(2_MB, nid, 4_KB);
        data_size_ = 2_GB;
        data_ = dsm_->alloc_from(2_GB, nid, 4_KB);
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }
    void init_client(BLS &bls, const Config &, CoroContext *ctx)
    {
        auto &cls = bls.coroutine();
        cls.buddy_ = std::make_shared<avis::BuddyAllocator>(
            dsm_, meta_, meta_size_, data_, data_size_, 4_KB, nullptr, ctx);
    }
    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        auto &cls = bls.coroutine();
        auto &id = bls.id();
        bool is_master = id.is_master_thread() && id.is_master_worker_coro();

        init_client(bls, conf, &ctx);

        auto &buddy = cls.buddy_;

        while (!token->stop_requested())
        {
            auto order = fast_pseudo_rand_int(8, 12);
            buddy->get_free_pages(order);
            token->complete_task(1);
        }
        if (is_master)
        {
            avis::avis_debug();
            std::vector<avis::BuddyProvider> providers;
            providers.emplace_back(avis::BuddyProvider{
                .node_id = (uint32_t) dsm_->get_node_id(),
                .meta_raddr = meta_,
                .meta_size = meta_size_,
                .buf_raddr = data_,
                .buf_size = data_size_,
                .page_size = 4_KB,
            });
            std::make_unique<avis::Dumper>(dsm_, providers)->report(true);
        }
    }

private:
    DSM::pointer dsm_;

    size_t meta_size_;
    GlobalAddress meta_;
    size_t data_size_;
    GlobalAddress data_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    Experiment exp;

    ::bench::ConfigFactory f;

    f.configure_thread_nr({16});
    f.configure_coro_nr({3});
    exp.configure_monitor(100ms, 20);
    exp.launch_coroutines(f.generate_configs());

    LOG(INFO) << "PASS.";
}