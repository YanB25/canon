#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>
#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "Common.h"
#include "DSMCache.h"
#include "GlobalAddress.h"
#include "Timer.h"
#include "WRLock.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
#include "util/Hexdump.hpp"
#include "util/ProcessMem.h"
#include "util/Rand.h"
#include "util/Util.h"
#include "util/gflags_def.h"
#include "util/thread_id.h"
DEFINE_string(msg, "hello workd", "the message");
DEFINE_uint32(cl_nr, 2, "the number of cacheline");

struct Spec
{
    // using TLS = ThreadContext;
    struct TLS
    {
        size_t ok{};
        size_t reorder{};
        std::set<size_t> split_at;
        size_t cqe_fast{};
        size_t cqe_slow{};
    };
};

class Buf
{
public:
    Buf(char *buf, size_t size) : buf_(buf), size_(size)
    {
    }
    std::set<size_t> split_at() const
    {
        std::set<size_t> ret;
        for (size_t i = 0; i < size_; ++i)
        {
            if (i != 0 && buf_[i] != buf_[i - 1])
            {
                ret.insert(i);
            }
        }
        return ret;
    }

private:
    char *buf_;
    size_t size_;
};

enum Order
{
    WW_CONFLICT,
    WR_ATOMIC,
};

std::ostream &operator<<(std::ostream &os, Order o)
{
    switch (o)
    {
    case Order::WW_CONFLICT:
        os << "WW_CONFLICT";
        break;
    case Order::WR_ATOMIC:
        os << "WR_ATOMIC";
        break;
    }
    return os;
}

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using BLS = typename Base::BLS;
    Experiment()
    {
        DSMConfig config;
        config.worker_nr = 0;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();

        CHECK_EQ(dsm_->getClusterSize(), 4)
            << "** It does not seem to be a correct configuration. If you "
               "believe it is correct, delete me.";

        if (is_server())
        {
            int fd = open("./file.txt", O_RDWR);
            PLOG_IF(FATAL, fd <= 0) << "** failed to open file.txt";

            mem_ = (char *) mmap(
                nullptr, 2_GB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            PLOG_IF(FATAL, mem_ == nullptr || mem_ == MAP_FAILED)
                << "** failed to mmap";

            auto dir_id = 0;
            auto *pd = dsm_->get_dir_rdma_context(dir_id)->pd;
            mr_ = CHECK_NOTNULL(ibv_reg_mr(
                pd,
                mem_,
                2_GB,
                IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_REMOTE_WRITE |
                    IBV_EXP_ACCESS_REMOTE_READ | IBV_EXP_ACCESS_REMOTE_ATOMIC));
            LOG(INFO) << PRE(*mr_, (void *) mr_->addr);

            if (is_master_server())
            {
                dsm_->put("master_rkey", mr_->rkey, 100ms);
                dsm_->put("master_addr", mem_, 100ms);
            }
            else
            {
                dsm_->put("side_rkey", mr_->rkey, 100ms);
                dsm_->put("side_addr", mem_, 100ms);
            }
        }
        master_rkey_ = dsm_->get<uint32_t>("master_rkey", 100ms);
        master_addr_ = dsm_->get<uint64_t>("master_addr", 100ms);
        side_rkey_ = dsm_->get<uint32_t>("side_rkey", 100ms);
        side_addr_ = dsm_->get<uint64_t>("side_addr", 100ms);
        LOG(INFO) << PRE(master_rkey_,
                         (void *) master_addr_,
                         side_rkey_,
                         (void *) side_addr_);
        // make sure they are on the same phm
        if (is_master_client())
        {
            GlobalAddress gaddr;
            gaddr.nodeID = master_server_nid_;
            gaddr.offset = 0;
            auto buf = dsm_->get_rdma_buffer(64);
            memset(buf.buffer, 'a', 64);
            auto *wr =
                dsm_->prepare_write(buf.buffer, gaddr, 64, false, nullptr);
            wr->wr.rdma.remote_addr = master_addr_;
            wr->wr.rdma.rkey = master_rkey_;
            dsm_->commit(nullptr);

            // okay read it back.
            gaddr.nodeID = side_server_nid_;
            memset(buf.buffer, 0, 64);
            wr = dsm_->prepare_read(buf.buffer, gaddr, 64, false, nullptr);
            wr->wr.rdma.remote_addr = side_addr_;
            wr->wr.rdma.rkey = side_rkey_;
            dsm_->commit(nullptr);
            for (size_t i = 0; i < 64; ++i)
            {
                CHECK_EQ(buf.buffer[i], 'a');
            }
        }
    }
    void exit() override
    {
        if (is_server())
        {
            int ret = ibv_dereg_mr(CHECK_NOTNULL(mr_));
            PLOG_IF(FATAL, ret < 0) << "** failed to dereg mr";
        }
    }

    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }

    bool is_server() const
    {
        auto nid = dsm_->get_node_id();
        return servers_.contains(nid);
    }
    bool is_client() const
    {
        auto nid = dsm_->get_node_id();
        return clients_.contains(nid);
    }
    bool is_master_client() const
    {
        auto nid = dsm_->get_node_id();
        return nid == master_client_nid_;
    }
    bool is_side_client() const
    {
        return is_client() && !is_master_client();
    }
    bool is_master_server() const
    {
        auto nid = dsm_->get_node_id();
        return nid == master_server_nid_;
    }
    bool is_side_server() const
    {
        return is_server() && !is_master_server();
    }
    void thread_init(BLS &bls, size_t thread_nr) override
    {
        dsm_->registerThread();
        Base::thread_init(bls, thread_nr);
    }
    void on_start_bench(BLS &bls, const Config &conf) override
    {
        auto order = conf.get<Order>("order");
        auto size = conf.get<size_t>("size");
        [[maybe_unused]] char ch1 = conf.get<char>("ch1");
        auto buf1 = dsm_->get_rdma_buffer(size);
        auto buf2 = dsm_->get_rdma_buffer(size);
        if (is_master_server())
        {
            if (order == WW_CONFLICT)
            {
                memset(mem_, 0, size);
            }
            else if (order == WR_ATOMIC)
            {
                memset(mem_, 0, size);
            }
            else
            {
                LOG(FATAL) << "Unknown order ";
            }
        }
        dsm_->put_rdma_buffer(std::move(buf1));
        dsm_->put_rdma_buffer(std::move(buf2));

        Base::on_start_bench(bls, conf);
    }

    void server(BLS &bls,
                const Config &conf,
                CoroContext &,
                ::bench::StopToken::pointer token)
    {
        std::ignore = bls;
        auto order = conf.get<Order>("order");
        auto size = conf.get<size_t>("size");

        size_t ok_nr = 0;
        size_t skip_nr = 0;

        std::set<size_t> split_at;

        while (!token->stop_requested())
        {
            // the first set of thread (odd tid)
            // check byte ordering
            if (order == WW_CONFLICT)
            {
                [[maybe_unused]] std::atomic<char> *pchar =
                    (std::atomic<char> *) mem_;
                std::vector<char> buf(size);
                util::memcpy_atomic_64b(buf.data(), mem_, size);

                auto split = Buf(buf.data(), buf.size()).split_at();
                split_at.merge(split);
                ok_nr++;

                token->complete_task(1);
            }
            else if (order == WR_ATOMIC)
            {
            }
            else
            {
                LOG(FATAL) << "Unknown order";
            }
        }
        LOG(INFO) << "OK: " << util::pre_num(ok_nr)
                  << ", skip: " << util::pre_num(skip_nr);
        LOG_IF(INFO, !split_at.empty()) << PRE(split_at);
        auto min_not_dividable = 512;
        for (size_t gcd : {8, 16, 32, 64, 128, 256, 512})
        {
            for (auto split : split_at)
            {
                if (split % gcd != 0)
                {
                    min_not_dividable = std::min(min_not_dividable, (int) gcd);
                }
            }
        }
        LOG_IF(ERROR, !split_at.empty())
            << "Not dividable by " << PRE(min_not_dividable);
    }
    constexpr static char min = 0;
    constexpr static char max = std::numeric_limits<char>::max();
    static char next(char c)
    {
        return c == max ? min : c + 1;
    }
    static bool is_next(char cur, char next_cur)
    {
        return next(cur) == next_cur;
    }
    static bool is_ge(char lhs, char rhs)
    {
        return lhs == rhs || is_gt(lhs, rhs);
    }
    static bool is_gt(char lhs, char rhs)
    {
        if (lhs == rhs)
        {
            return false;
        }
        DCHECK_GE((int) lhs, 0);
        DCHECK_GE((int) rhs, 0);
        // int rhs_minus_lhs = (int) rhs - (int) lhs;
        // int lhs_minus_rhs =
        int inner_distance = std::max(lhs, rhs) - std::min(lhs, rhs);
        int outer_distance =
            std::min((int) lhs, (int) rhs) + max - std::max(lhs, rhs);
        CHECK_EQ(inner_distance + outer_distance, max);
        // they are closer
        // LOG(INFO) << PRE(inner_distance, outer_distance);
        if (inner_distance <= outer_distance)
        {
            return lhs > rhs;
        }
        else
        {
            // they cross the boundary
            return lhs < rhs;
        }
    }

    void client(BLS &bls,
                const Config &conf,
                CoroContext &ctx,
                ::bench::StopToken::pointer token)
    {
        auto order = conf.get<Order>("order");
        auto size = conf.get<size_t>("size");
        // bool fence = conf.get<bool>("fence");
        auto id = bls.id().thread_id * conf.coro_nr() + bls.id().coro_id;
        bool group_1 = id % 2 == 0;
        bool group_2 = id % 2 == 1;

        auto gaddr1 = dsm_->alloc(size);
        auto gaddr2 = dsm_->alloc(size);

        auto &tls = bls.thread();
        char ch1 = conf.get<char>("ch1");
        char ch2 = next(ch1);

        // init
        auto buf1 = dsm_->get_rdma_buffer(size);
        auto buf2 = dsm_->get_rdma_buffer(size);
        auto buf3 = dsm_->get_rdma_buffer(size);
        auto buf4 = dsm_->get_rdma_buffer(size);
        memset(buf1.buffer, ch1, size);
        memset(buf2.buffer, ch2, size);
        memset(buf3.buffer, 0, size);
        memset(buf4.buffer, 0, size);

        CHECK(is_client());

        while (!token->stop_requested())
        {
            if (order == WW_CONFLICT)
            {
                auto ch = fast_pseudo_rand_int(min, max);
                if (group_1)
                {
                    GlobalAddress gaddr1;
                    gaddr1.nodeID = master_server_nid_;
                    gaddr1.offset = 0;

                    memset(buf1.buffer, ch, size);

                    auto *wr1 = dsm_->prepare_write(
                        buf1.buffer, gaddr1, size, false, &ctx);
                    wr1->wr.rdma.remote_addr = master_addr_;
                    wr1->wr.rdma.rkey = master_rkey_;
                }
                else if (group_2)
                {
                    GlobalAddress gaddr2;
                    gaddr2.nodeID = side_server_nid_;
                    gaddr2.offset = 0;

                    memset(buf2.buffer, ch, size);
                    auto *wr2 = dsm_->prepare_write(
                        buf2.buffer, gaddr2, size, false, &ctx);
                    wr2->wr.rdma.remote_addr = side_addr_;
                    wr2->wr.rdma.rkey = side_rkey_;
                }
                else
                {
                    LOG(FATAL);
                }

                dsm_->commit(&ctx);

                token->complete_task(1);
            }
            else if (order == WR_ATOMIC)
            {
                auto ch = fast_pseudo_rand_int(min, max);
                if (group_1)
                {
                    GlobalAddress gaddr1;
                    gaddr1.nodeID = master_server_nid_;
                    gaddr1.offset = 0;

                    memset(buf1.buffer, ch, size);

                    auto *wr1 = dsm_->prepare_write(
                        buf1.buffer, gaddr1, size, false, &ctx);
                    wr1->wr.rdma.remote_addr = master_addr_;
                    wr1->wr.rdma.rkey = master_rkey_;

                    auto *wr3 = dsm_->prepare_read(
                        buf3.buffer, gaddr1, size, false, &ctx);
                    wr3->wr.rdma.remote_addr = master_addr_;
                    wr3->wr.rdma.rkey = master_rkey_;
                }
                else if (group_2)
                {
                    GlobalAddress gaddr2;
                    gaddr2.nodeID = side_server_nid_;
                    gaddr2.offset = 0;

                    memset(buf2.buffer, ch, size);

                    auto *wr2 = dsm_->prepare_write(
                        buf2.buffer, gaddr2, size, false, &ctx);
                    wr2->wr.rdma.remote_addr = side_addr_;
                    wr2->wr.rdma.rkey = side_rkey_;

                    auto *wr4 = dsm_->prepare_read(
                        buf4.buffer, gaddr2, size, false, &ctx);
                    wr4->wr.rdma.remote_addr = side_addr_;
                    wr4->wr.rdma.rkey = side_rkey_;
                }

                dsm_->commit(&ctx);

                tls.split_at.merge(Buf(buf3.buffer, size).split_at());
                tls.split_at.merge(Buf(buf4.buffer, size).split_at());

                token->complete_task(1);
            }
            else
            {
                LOG(FATAL) << "Unknown order";
            }
        }
        LOG(INFO) << PRE(tls.ok, tls.reorder, tls.split_at.size());
        // LOG(INFO) << PRE(tls.reorder | ranges::views::take(10));
        auto min_not_dividable = 512;
        for (size_t gcd : {8, 16, 32, 64, 128, 256, 512})
        {
            for (auto split : tls.split_at)
            {
                if (split % gcd != 0)
                {
                    min_not_dividable = std::min(min_not_dividable, (int) gcd);
                }
            }
        }
        LOG_IF(ERROR, !tls.split_at.empty())
            << "Not dividable by " << PRE(min_not_dividable);
        std::vector<size_t> split_at_10;
        for (size_t i = 0; i < 10; ++i)
        {
            if (!tls.split_at.empty())
            {
                auto r = *tls.split_at.begin();
                tls.split_at.erase(r);
                split_at_10.push_back(r);
            }
        }
        LOG_IF(INFO, !split_at_10.empty()) << PRE(split_at_10);
        LOG_IF(INFO, tls.cqe_slow) << "slow: " << util::pre_num(tls.cqe_slow)
                                   << ", ok: " << util::pre_num(tls.cqe_fast);

        dsm_->free(gaddr1, size);
        dsm_->free(gaddr2, size);
        dsm_->put_rdma_buffer(std::move(buf1));
        dsm_->put_rdma_buffer(std::move(buf2));
        dsm_->put_rdma_buffer(std::move(buf3));
        dsm_->put_rdma_buffer(std::move(buf4));
    }

    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        if (is_server())
        {
            server(bls, conf, ctx, token);
        }
        else if (is_client())
        {
            client(bls, conf, ctx, token);
        }
        else
        {
            LOG(INFO) << "Idle";
        }

        while (!token->stop_requested())
        {
            std::this_thread::sleep_for(10ms);
        }
    }

private:
    DSM::pointer dsm_;

    std::set<size_t> clients_{0, 1};
    std::set<size_t> servers_{2, 3};
    size_t master_client_nid_{0};
    size_t side_server_nid_{2};
    size_t master_server_nid_{3};

    uint32_t master_rkey_;
    uint64_t master_addr_;
    uint32_t side_rkey_;
    uint64_t side_addr_;

    ibv_mr *mr_{};  // used only for resource management
    char *mem_{};   // only valid when is_server() is true.
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    LOG(INFO) << "Please use this configuration:";
    LOG(INFO) << "4 nodes. 0 and 1 nodes are in the same machine (two numa, as "
                 "client), 2 "
                 "and 3 nodes are in the same machine (two numa, as server)";

    bench::ConfigFactory f;
    f.configure_thread_nr({24});
    f.configure_coro_nr({4});

    // f.add_option<bool>("fence", {true, false});
    f.add_option<bool>("fence", {false});

    // f.add_option<Order>("order", {RW, WR});
    // f.add_option<Order>("order", {WR});
    // f.add_option<Order>("order", {RW});
    f.add_option<Order>("order", {WR_ATOMIC});
    // f.add_option<Order>("order", {LocalR_Order});
    f.add_option<size_t>("size", {64_B, 128_B, 256_B, 4_KB});
    // f.add_option<size_t>("size", {256, 4_KB});
    f.add_option<char>("ch1", {'a'});
    // f.add_option<Order>("order", {RR});
    auto configs = f.generate_configs();

    exp.configure_monitor(200ms, 20);
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}

// RR
// tls: {Spec::TLS <229326, 1, {3520, 3584}>}
// tls: {Spec::TLS <229286, 0, {}>}
// tls: {Spec::TLS <229302, 1, {1920, 1984}>}
// tls: {Spec::TLS <229338, 1, {3712, 3776}>}
// Not dividable by min_not_dividable: 128
// tls: {Spec::TLS <229298, 0, {}>}
// tls: {Spec::TLS <229290, 0, {}>}
// Not dividable by min_not_dividable: 128
// tls: {Spec::TLS <229288, 0, {}>}
// Not dividable by min_not_dividable: 128
// tls: {Spec::TLS <229170, 4, {2688, 2752, 3072, 3136, 3200, 3264, 3328}>}
// Not dividable by min_not_dividable: 128

// RW
// tls: {Spec::TLS <229692, 3, {1152, 1984, 2048, 2176}>}
// tls: {Spec::TLS <229678, 2, {1216, 1280, 3840, 3904}>}
// Not dividable by min_not_dividable: 128
// Not dividable by min_not_dividable: 128