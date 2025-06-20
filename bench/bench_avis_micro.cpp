#include <chrono>
#include <thread>

#include "Common.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "avis/ARC.h"
#include "avis/DTxn.h"
#include "avis/avis.h"
#include "avis/handle.h"
#include "avis/manager.h"
#include "bench/experiment.h"
#include "bench/request.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "sherman/Tree.h"
#include "util/PerformanceReporter.h"
#include "util/PreUtil.h"
#include "util/Rand.h"
#include "util/Util.h"
#include "util/gflags_dec.h"
#include "util/gflags_def.h"

using Provider = avis::BuddyProvider;

struct Spec
{
    struct CLS
    {
        std::shared_ptr<avis::arc::ARC> arc;
        std::shared_ptr<avis::AvisHandle> handle;
        std::shared_ptr<avis::DTxn> dtxn;
        uint64_t gcid{};
    };
};

enum Who
{
    kAvis,
    kARC,
    kDTxn,
    kPartition
};
inline std::ostream &operator<<(std::ostream &os, const Who &h)
{
    switch (h)
    {
    case kAvis:
        os << "kAvis";
        break;
    case kARC:
        os << "kARC";
        break;
    case kDTxn:
        os << "kDTxn";
        break;
    case kPartition:
        os << "kPartition";
        break;
    }
    return os;
}

struct Dist
{
    double put_rate;
    double del_rate;
};
inline std::ostream &operator<<(std::ostream &os, const Dist &d)
{
    os << fmt::format("{}-{}", d.put_rate, d.del_rate);
    return os;
}

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

        server_nid_ = dsm_->getClusterSize() - 1;
        M_ = 32 * 3 * 4;

        if (is_server())
        {
            m_ = std::make_unique<avis::AvisManager>(
                dsm_, 4 /* buddy nr */, 2_MB /* pub */, M_ /* client_nr */);
            // sync providers
            const auto &providers = m_->providers();
            dsm_->put("size", (uint32_t) providers.size(), 100ms);
            dsm_->put("providers",
                      providers.data(),
                      providers.size() * sizeof(Provider),
                      100ms);
            auto arc_size = 2_MB;
            auto arc = dsm_->alloc_from(2_MB, server_nid_, 8);
            dsm_->put("arc", arc, 100ms);
            dsm_->put("arc_size", arc_size, 100ms);
        }
        auto size = dsm_->get<uint32_t>("size", 100ms);
        providers_.resize(size);
        auto *raw = dsm_->get_raw("providers", 100ms);
        memcpy(providers_.data(), raw, providers_.size() * sizeof(Provider));

        pub_meta_ = dsm_->get<GlobalAddress>("pub", 100ms);
        pub_size_ = dsm_->get<size_t>("pub_size", 100ms);
        arc_meta_ = dsm_->get<GlobalAddress>("arc", 100ms);
        arc_size_ = dsm_->get<size_t>("arc_size", 100ms);
    }
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }
    bool is_server() const
    {
        return dsm_->get_node_id() == server_nid_;
    }
    bool is_client() const
    {
        return !is_server();
    }
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        bool ptl = conf.get<bool>("enable_ptl");
        bool bp = conf.get<bool>("enable_bp");
        auto who = conf.get<Who>("who");
        if (who == kAvis)
        {
            avis::Config::ins().configure_use_partition_allocator(false);
        }
        else
        {
            // avis::Config::ins().configure_use_partition_allocator(true);
        }

        avis::Config::ins().configure_ptl(ptl);
        avis::Config::ins().configure_bp(bp);

        Base::on_start_bench(bls, conf);
    }
    void init_client(BLS &bls, const Config &config, CoroContext *ctx)
    {
        auto &cls = bls.coroutine();
        auto request_config =
            config.get<bench::RequestGenerator::Config>("request_config");
        auto who = config.get<Who>("who");
        auto &id = bls.id();
        auto tid = id.thread_id;
        auto cid = id.coro_id;
        auto nid = dsm_->get_node_id();
        auto global_id = nid * (32 * 3) + tid * (3) + cid;
        cls.gcid = global_id;

        // LOG(INFO) << PRE(global_id);

        auto ptl = std::make_shared<avis::PTL>(dsm_, server_nid_, ctx);
        auto pub = std::make_shared<avis::Publisher<avis::BitmapPub>>(
            dsm_, pub_meta_, pub_size_, ctx);
        auto handle =
            std::make_shared<avis::AvisHandle>(providers_, dsm_, ptl, pub, ctx);

        cls.handle = handle;

        if (who == Who::kARC)
        {
            auto arc = std::make_shared<avis::arc::ARC>(
                dsm_, M_, global_id, arc_meta_, arc_size_, ptl, ctx);
            cls.arc = arc;
        }
        if (who == Who::kDTxn)
        {
            auto lock_size = 2_MB;
            auto locks = handle->alloc(lock_size);
            auto data_size = 256_MB;
            auto data = handle->alloc(data_size);
            auto bitmap_size = 2_MB;
            auto bitmap = handle->alloc(bitmap_size);
            auto dtxn = std::make_shared<avis::DTxn>(dsm_,
                                                     locks,
                                                     lock_size,
                                                     data,
                                                     data_size,
                                                     bitmap,
                                                     bitmap_size,
                                                     ctx);
            cls.dtxn = dtxn;
        }
    }

    void avis_do(std::shared_ptr<avis::AvisHandle> &handle,
                 const Config &conf,
                 CoroContext *ctx,
                 GlobalAddress meta)
    {
        const auto &request_config =
            conf.get<bench::RequestGenerator::Config>("request_config");
        bench::RequestGenerator g(request_config);

        auto [put_rate, get_rate, del_rate] = request_config.config.put_get_del;
        double select = fast_pseudo_rand_dbl(0, 1);
        bool is_put = select < put_rate;
        // bool is_get = select < (put_rate + get_rate);
        bool is_del = true;

        auto slot_id = g.next_key();
        GlobalAddress slot_gaddr = meta + slot_id * sizeof(GlobalAddress);
        auto value_size = g.next_value_size();

        if (is_put)
        {
            // Need BP + size
            auto raddr = handle->alloc(value_size + 16 + 8);
            raddr.offset = util::round_up_aligned(raddr.offset, 8);
            CHECK(!raddr.is_null());

            auto rdma_buf = dsm_->get_rdma_buffer(value_size + 16);
            fast_pseudo_fill_buf(rdma_buf.buffer, value_size + 16);
            uint64_t *pu64 = (uint64_t *) rdma_buf.buffer;
            pu64[0] = slot_gaddr.val;
            pu64[1] = value_size + 16;
            dsm_->prepare_write(
                rdma_buf.buffer, raddr, value_size + 8, false, ctx);

            auto slot_buf = dsm_->get_rdma_buffer(sizeof(GlobalAddress));
            memcpy(slot_buf.buffer, &raddr.val, sizeof(GlobalAddress));

            // leverage W-W ordering
            dsm_->prepare_write(
                slot_buf.buffer, slot_gaddr, sizeof(GlobalAddress), false, ctx);

            dsm_->commit(ctx);

            dsm_->put_rdma_buffer(std::move(rdma_buf));
            dsm_->put_rdma_buffer(std::move(slot_buf));
        }
        else if (is_del)
        {
            auto slot_buf = dsm_->get_rdma_buffer(sizeof(GlobalAddress));
            dsm_->prepare_read(
                slot_buf.buffer, slot_gaddr, sizeof(GlobalAddress), false, ctx);
            dsm_->commit(ctx);

            auto gaddr = *(GlobalAddress *) slot_buf.buffer;
            if (!gaddr.is_null())
            {
                auto bp_size_buf = dsm_->get_rdma_buffer(16);
                dsm_->prepare_read(bp_size_buf.buffer, gaddr, 16, false, ctx);
                dsm_->commit(ctx);
                uint64_t *pu64 = (uint64_t *) bp_size_buf.buffer;
                size_t size = pu64[1];
                handle->free(gaddr, size);
                dsm_->put_rdma_buffer(std::move(bp_size_buf));
            }

            dsm_->put_rdma_buffer(std::move(slot_buf));
        }
        else
        {
            LOG(FATAL) << "TODO: we do not need get";
        }
    }
    void arc_do(uint64_t gcid,
                std::shared_ptr<avis::AvisHandle> &handle,
                std::shared_ptr<avis::arc::ARC> &arc,
                const Config &conf,
                CoroContext *ctx,
                GlobalAddress meta)
    {
        const auto &request_config =
            conf.get<bench::RequestGenerator::Config>("request_config");
        bench::RequestGenerator g(request_config);

        auto [put_rate, get_rate, del_rate] = request_config.config.put_get_del;
        double select = fast_pseudo_rand_dbl(0, 1);
        bool is_put = select < put_rate;
        // bool is_get = select < (put_rate + get_rate);
        bool is_del = true;

        auto slot_id = g.next_key();
        GlobalAddress slot_gaddr = meta + slot_id * sizeof(GlobalAddress);
        auto value_size = g.next_value_size();

        if (is_put)
        {
            auto raddr = handle->alloc(value_size + 16 + 8);  // header and size
            CHECK(!raddr.is_null());
            raddr.offset = util::round_up_aligned(raddr.offset, 8);
            auto rdma_buf = dsm_->get_rdma_buffer(value_size + 16);
            fast_pseudo_fill_buf(rdma_buf.buffer, value_size + 16);
            uint64_t *pu64 = (uint64_t *) rdma_buf.buffer;
            avis::arc::Header hdr{
                .lcid = (uint16_t) gcid, .lera = 0, .ref_cnt = 0};
            pu64[0] = *(uint64_t *) &hdr;
            pu64[1] = value_size + 16;
            arc->AttachReference(slot_gaddr, raddr);

            dsm_->put_rdma_buffer(std::move(rdma_buf));
        }
        else if (is_del)
        {
            auto slot_buf = dsm_->get_rdma_buffer(sizeof(GlobalAddress));
            dsm_->prepare_read(
                slot_buf.buffer, slot_gaddr, sizeof(GlobalAddress), false, ctx);
            dsm_->commit(ctx);

            auto gaddr = *(GlobalAddress *) slot_buf.buffer;
            if (!gaddr.is_null())
            {
                auto hdr_size_buf = dsm_->get_rdma_buffer(16);
                dsm_->prepare_read(hdr_size_buf.buffer, gaddr, 16, false, ctx);
                dsm_->commit(ctx);
                // uint64_t *pu64 = (uint64_t *) hdr_size_buf.buffer;
                arc->DetachReference(slot_gaddr, gaddr);
            }
        }
        else
        {
            LOG(FATAL) << "TODO: we do not need get";
        }
    }

    void dtxn_do(std::shared_ptr<avis::DTxn> &dtxn,
                 const Config &conf,
                 CoroContext *,
                 GlobalAddress meta)
    {
        const auto &request_config =
            conf.get<bench::RequestGenerator::Config>("request_config");
        bench::RequestGenerator g(request_config);

        auto [put_rate, get_rate, del_rate] = request_config.config.put_get_del;
        double select = fast_pseudo_rand_dbl(0, 1);
        bool is_put = select < put_rate;
        // bool is_get = select < (put_rate + get_rate);
        bool is_del = true;

        auto slot_id = g.next_key();
        [[maybe_unused]] GlobalAddress slot_gaddr =
            meta + slot_id * sizeof(GlobalAddress);
        auto value_size = g.next_value_size();

        if (is_put)
        {
            // auto rdma_buf = dsm_->get_rdma_buffer(value_size + 16 + 8);

            [[maybe_unused]] auto raddr = dtxn->alloc(value_size + 16 + 8);
            std::vector<char> buf(value_size + 16 + 8);
            fast_pseudo_fill_buf(buf.data(), buf.size());
            dtxn->rdma_write(raddr, buf.data(), buf.size());
            dtxn->rdma_write(slot_gaddr, (char *) &raddr.val, sizeof(raddr));
            dtxn->commit();

            // dsm_->put_rdma_buffer(std::move(rdma_buf));
        }
        else if (is_del)
        {
            LOG(FATAL) << "TODO:";
        }
        else
        {
            LOG(FATAL) << "TODO: we do not need get";
        }
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

        auto who = conf.get<Who>("who");
        // bool verbose = conf.get<bool>("verbose");

        const auto &request_config =
            conf.get<bench::RequestGenerator::Config>("request_config");
        auto meta_size = request_config.config.key_rng * sizeof(GlobalAddress);
        auto meta = dsm_->alloc_from(meta_size, server_nid_, 8);
        auto buf = dsm_->get_rdma_buffer(meta_size);
        memset(buf.buffer, 0, meta_size);
        dsm_->prepare_write(buf.buffer, meta, meta_size, false, &ctx);
        dsm_->commit(&ctx);
        dsm_->put_rdma_buffer(std::move(buf));

        init_client(bls, conf, &ctx);

        ChronoTimer timer(is_master /* enable */);

        auto &handle = cls.handle;
        auto &arc = cls.arc;

        auto min = std::chrono::nanoseconds(0ns).count();
        auto max = std::chrono::nanoseconds(1ms).count();
        auto step = std::chrono::nanoseconds(100ns).count();
        OnePassBucketMonitor<uint64_t> lat(min, max, step);

        GlobalAddress g_raddr;
        if (who == kPartition)
        {
            g_raddr = handle->alloc(2_MB);
        }

        while (!token->stop_requested())
        {
            if (is_client())
            {
                timer.pin();

                if (who == kAvis)
                {
                    avis_do(handle, conf, &ctx, meta);
                }
                else if (who == kARC)
                {
                    arc_do(cls.gcid, handle, arc, conf, &ctx, meta);
                }
                else if (who == kDTxn)
                {
                    dtxn_do(cls.dtxn, conf, &ctx, meta);
                }
                else if (who == kPartition)
                {
                    auto raddr =
                        g_raddr + fast_pseudo_rand_int(0, 2_MB / 8) * 8;
                    partition_do(conf, &ctx, raddr, meta);
                }
                else
                {
                    LOG(FATAL) << "** unknown " << (int) who;
                }
                token->complete_task(1);
                auto ns = timer.pin();
                lat.collect(ns);
            }
        }
        if (is_master)
        {
            LOG(INFO) << PRE(lat);
        }
    }

    void partition_do(const Config &conf,
                      CoroContext *ctx,
                      GlobalAddress raddr,
                      GlobalAddress meta)
    {
        const auto &request_config =
            conf.get<bench::RequestGenerator::Config>("request_config");
        bench::RequestGenerator g(request_config);

        auto [put_rate, get_rate, del_rate] = request_config.config.put_get_del;
        double select = fast_pseudo_rand_dbl(0, 1);
        bool is_put = select < put_rate;
        // bool is_get = select < (put_rate + get_rate);
        bool is_del = true;

        auto slot_id = g.next_key();
        GlobalAddress slot_gaddr = meta + slot_id * sizeof(GlobalAddress);
        auto value_size = g.next_value_size();

        if (is_put)
        {
            auto rdma_buf = dsm_->get_rdma_buffer(value_size + 16);
            fast_pseudo_fill_buf(rdma_buf.buffer, value_size + 16);
            uint64_t *pu64 = (uint64_t *) rdma_buf.buffer;
            pu64[0] = value_size;
            dsm_->prepare_write(
                rdma_buf.buffer, raddr, value_size + 8, false, ctx);

            auto slot_buf = dsm_->get_rdma_buffer(sizeof(GlobalAddress));
            memcpy(slot_buf.buffer, &raddr.val, sizeof(GlobalAddress));

            // leverage W-W ordering
            dsm_->prepare_write(
                slot_buf.buffer, slot_gaddr, sizeof(GlobalAddress), false, ctx);

            dsm_->commit(ctx);

            dsm_->put_rdma_buffer(std::move(rdma_buf));
            dsm_->put_rdma_buffer(std::move(slot_buf));
        }
        else if (is_del)
        {
            auto slot_buf = dsm_->get_rdma_buffer(sizeof(GlobalAddress));
            dsm_->prepare_read(
                slot_buf.buffer, slot_gaddr, sizeof(GlobalAddress), false, ctx);
            dsm_->commit(ctx);

            auto gaddr = *(GlobalAddress *) slot_buf.buffer;
            if (!gaddr.is_null())
            {
                auto bp_size_buf = dsm_->get_rdma_buffer(8);
                dsm_->prepare_read(bp_size_buf.buffer, gaddr, 8, false, ctx);
                dsm_->commit(ctx);
                uint64_t *pu64 = (uint64_t *) bp_size_buf.buffer;
                size_t size = pu64[0];
                std::ignore = size;
                dsm_->put_rdma_buffer(std::move(bp_size_buf));
            }

            dsm_->put_rdma_buffer(std::move(slot_buf));
        }
        else
        {
            LOG(FATAL) << "TODO: we do not need get";
        }
    }

    void on_end_bench(const ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        // bool verbose = conf.get<bool>("verbose");
        auto who = conf.get<Who>("who");
        if (is_client())
        {
            if (who == kAvis)
            {
                auto dumper = std::make_shared<avis::Dumper>(dsm_, providers_);
                dumper->report(false);
                auto [used, total] = dumper->bitmap_utilizations();
                LOG(INFO) << PRE(used, total);
            }
        }
        Base::on_end_bench(res, bls, conf);
    }

private:
    DSM::pointer dsm_;

    GlobalAddress pub_meta_;
    size_t pub_size_;

    GlobalAddress arc_meta_;
    size_t arc_size_;

    size_t server_nid_;
    std::vector<Provider> providers_;

    // server only
    std::unique_ptr<avis::AvisManager> m_;

    size_t M_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    Experiment exp;

    ::bench::ConfigFactory f;

    // f.configure_thread_nr({1, 2, 4, 8, 12, 16, 20, 24, 28, 32});
    // f.configure_thread_nr({1});
    // f.configure_thread_nr({1, 2, 4, 8});
    // f.configure_thread_nr({12, 16});
    // f.configure_thread_nr({20, 24});
    // f.configure_thread_nr({20, 24, 28, 32});
    // f.configure_thread_nr({28, 32});

    f.configure_thread_nr({32});
    f.configure_coro_nr({3});

    bench::RequestGenerator::Config request_config{
        .config{
            .put_get_del = {1.0, 0, 0},
            .key_size_dist = {{8, 1.0}},
            .z = 0,
            .key_rng = 2048,
        },
        .value_size_dist =
            std::vector<std::pair<size_t, double>>{
                {16, 0.2}, {64, 0.2}, {128, 0.2}, {256, 0.2}, {512, 0.2}},
        // {{16, 1}},
        .value_size_model = {},
    };

    f.add_option<bench::RequestGenerator::Config>("request_config",
                                                  {request_config});
    f.add_option<bool>("verbose", {false});
    f.add_option<bool>("enable_bp", {false});
    f.add_option<bool>("enable_ptl", {false});
    // f.add_option<Who>("who", {kAvis});
    // f.add_option<Who>("who", {kARC});
    // f.add_option<Who>("who", {kPartition});
    f.add_option<Who>("who", {kAvis});

    exp.configure_monitor(30ms, 10);
    exp.launch_coroutines(f.generate_configs());

    LOG(INFO) << "PASS.";
}