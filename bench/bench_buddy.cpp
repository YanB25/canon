#include <numa.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <thread>

#include "Common.h"
#include "DSMCache.h"
#include "GlobalAddress.h"
#include "GlobalAllocator.h"
#include "avis/avis.h"
#include "avis/buddy.h"
#include "avis/config.h"
#include "avis/ptl.h"
#include "bench/DataFrame.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Rand.h"
#include "util/bits.h"
#include "util/gflags_def.h"

struct Spec
{
    struct CLS
    {
        std::unique_ptr<avis::BuddyAllocator> alloc;
    };
};

struct OrderPair
{
    int lower;
    int upper;
};
std::ostream &operator<<(std::ostream &os, const OrderPair &p)
{
    os << fmt::format("{}-{}", p.lower, p.upper);
    return os;
}

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using BLS = typename Base::BLS;
    Experiment(DSM::pointer dsm, size_t total_size, size_t page_size)
        : dsm_(dsm), total_size_(total_size), page_size_(page_size)
    {
        dsm_->registerThread();
        server_nid_ = dsm_->getClusterSize() - 1;
        auto nid = dsm_->get_node_id();
        if (nid == 0)
        {
            auto meta = dsm_->alloc_from(2_MB, server_nid_, 4_KB /* align */);
            auto data =
                dsm_->alloc_from(total_size, server_nid_, 4_KB /* align */);
            dsm_->put("meta", meta, 100ms);
            dsm_->put("data", data, 100ms);
        }
        meta_ = dsm_->get<GlobalAddress>("meta", 100ms);
        data_ = dsm_->get<GlobalAddress>("data", 100ms);
        avis::Buddy b(total_size, page_size);
        LOG(INFO) << PRE(total_size, page_size, b.depth(), b.meta_bytes());

        auto rdma_buf = dsm_->get_rdma_buffer(2_MB);
        memset(rdma_buf.buffer, 0, 2_MB);
        dsm_->prepare_write(rdma_buf.buffer, meta_, 2_MB, false, nullptr);
        dsm_->commit();
        dsm_->put_rdma_buffer(std::move(rdma_buf));
    }
    void thread_init(BLS &bls, size_t thread_nr) override
    {
        dsm_->registerThread();
        Base::thread_init(bls, thread_nr);
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        auto mode = conf.get<avis::BuddyMode>("mode");
        bool postorder = conf.get<bool>("post_order");
        avis::Config::ins().configure_buddy_mode(mode);
        avis::Config::ins().configure_buddy_use_post_order(postorder);
        Base::on_start_bench(bls, conf);
    }
    void on_end_bench(const ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        // LOG(INFO) << "Validating...";
        // auto buddy = make_buddy(nullptr /* ctx */);
        // buddy->read_meta();
        // auto [ok, err] = buddy->get_allocated();
        // bool has_error = false;
        // if (!err.empty())
        // {
        //     LOG(ERROR) << "ERR: " << util::pre(err);
        //     has_error = true;
        // }
        // if (!ok.empty())
        // {
        //     LOG(ERROR) << "Memory not freed properly: " << std::endl
        //                << PRE(ok.size());
        //     has_error = true;
        // }
        // if (has_error)
        // {
        //     LOG(FATAL) << "Die";
        // }

        LOG(INFO) << "Resetting";
        auto rdma_buf = dsm_->get_rdma_buffer(2_MB);
        memset(rdma_buf.buffer, 0, 2_MB);
        dsm_->prepare_write(rdma_buf.buffer, meta_, 2_MB, false, nullptr);
        dsm_->commit(nullptr);
        dsm_->put_rdma_buffer(std::move(rdma_buf));

        df_.reg_result(res, conf);

        Base::on_end_bench(res, bls, conf);
    }

    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        Base::on_thread_end_bench(bls, conf);
    }

    void exit() override
    {
        df_.dump(FLAGS_binary, FLAGS_exec_meta);
    }

    std::unique_ptr<avis::BuddyAllocator> make_buddy(CoroContext *ctx)
    {
        auto ptl = std::make_shared<avis::PTL>(dsm_, server_nid_, ctx);
        return std::make_unique<avis::BuddyAllocator>(dsm_,
                                                      meta_,
                                                      2_MB /* meta_size */,
                                                      data_,
                                                      total_size_,
                                                      page_size_,
                                                      ptl,
                                                      ctx);
    }

    bool is_server()
    {
        return dsm_->get_node_id() == dsm_->getClusterSize() - 1;
    }
    bool is_client()
    {
        // return !is_server();
        return dsm_->get_node_id() == 0;
    }

    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        std::ignore = bls;
        auto &id = bls.id();
        bool is_master = id.is_master_thread() && id.is_master_worker_coro();
        auto &cls = bls.coroutine();
        auto [min_order, max_order] = conf.get<OrderPair>("order");
        cls.alloc = make_buddy(&ctx);
        auto &buddy = cls.alloc;

        bool alloc_phrase = true;
        std::unordered_map<GlobalAddress, unsigned> addrs;
        [[maybe_unused]] size_t allocated_nr = 0;
        [[maybe_unused]] size_t freed_nr = 0;
        while (!token->stop_requested())
        {
            if (!is_client())
            {
                continue;
            }
            int order = 0;
            if (min_order == max_order)
            {
                order = min_order;
            }
            else
            {
                order = fast_pseudo_rand_int(min_order, max_order);
            }
            if (alloc_phrase)
            {
                auto addr = buddy->get_free_pages(order);
                // LOG(INFO) << "alloc " << PRE(addr) << " from " << ctx;
                if (likely(!addr.is_null()))
                {
                    allocated_nr++;
                    bool succ = addrs.emplace(addr, order).second;
                    DCHECK(succ)
                        << "** " << PRE(addr) << " allocation duplicated";
                    token->complete_task(1);
                }
                else
                {
                    // LOG(INFO) << "Switch to free phase";
                    // no mem
                    alloc_phrase = false;
                }
            }
            else
            {
                // LOG(INFO) << "Free phase for " << addrs.size();
                while (!addrs.empty())
                {
                    auto [addr, order] = *addrs.begin();
                    addrs.erase(addr);
                    // LOG(INFO) << "Free " << PRE(addr) << " from " << ctx;
                    buddy->put_free_pages(addr, order);
                    freed_nr++;
                }
                alloc_phrase = true;
                // LOG(INFO) << "alloc phase";
            }
        }

        if (is_master)
        {
            auto lat_dump = buddy->dump_latency();
            for (const auto &[order, lat] : lat_dump)
            {
                LOG(INFO) << PRE(order, lat);
            }
        }
        // LOG(INFO) << "Draining... for size " << PRE(addrs.size());
        // while (!addrs.empty())
        // {
        //     auto addr = *addrs.begin();
        //     addrs.erase(addr);
        //     buddy->put_free_pages(addr);
        //     freed_nr++;
        // }
        // LOG(INFO) << "Leaving...: " << PRE(allocated_nr, freed_nr);
    }

private:
    DSM::pointer dsm_;
    GlobalAddress meta_;
    GlobalAddress data_;
    size_t total_size_;
    size_t page_size_;

    bench::ResultDataFrame df_;

    size_t server_nid_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    DSMConfig config;
    config.worker_nr = 0;
    auto dsm = DSM::getInstance(config);
    Experiment exp(dsm, 2_GB, 4_KB);

    bench::ConfigFactory f;
    f.configure_thread_nr({1, 4, 8, 16, 32});
    // f.configure_thread_nr({1, 2, 4, 8, 16, kMaxAppThread});
    // f.configure_thread_nr({1, 4, 8, 16, kMaxAppThread});
    // f.configure_thread_nr({kMaxAppThread});
    f.configure_coro_nr({3});
    // f.configure_coro_nr({1, 3});
    // [min, max] order
    f.add_option<avis::BuddyMode>("mode", {avis::BuddyMode::kRand});
    f.add_option<bool>("post_order", {true});
    // f.add_option<OrderPair>("order", {{0, 8}});
    // f.add_option<OrderPair>("order", {{5, 9}});
    f.add_option<OrderPair>("order", {{5, 5}});
    // f.add_option<std::pair<int, int>>("order", {{0, 9}});

    auto configs = f.generate_configs();

    exp.configure_monitor(20ms, 10);
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}