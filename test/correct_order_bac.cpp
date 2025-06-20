#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "DSMCache.h"
#include "WRLock.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
#include "util/gflags_def.h"
DEFINE_string(msg, "hello workd", "the message");
DEFINE_uint32(cl_nr, 2, "the number of cacheline");

struct Spec
{
    // using TLS = ThreadContext;
    struct TLS
    {
        struct RW
        {
            size_t ok{};
            size_t reorder{};
            std::set<size_t> split_at;
        } rw;
    };
};

enum Order
{
    WW,
    WR,
    RW,
    RR
};

std::ostream &operator<<(std::ostream &os, Order o)
{
    switch (o)
    {
    case Order::WW:
        os << "WW";
        break;
    case Order::WR:
        os << "WR";
        break;
    case Order::RW:
        os << "RW";
        break;
    case Order::RR:
        os << "RR";
        break;
    }
    return os;
}

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    using BLS = typename Base::BLS;
    Experiment()
    {
        DSMConfig config;
        config.worker_nr = 0;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();

        if (dsm_->get_node_id() == server_nid_)
        {
            auto base = dsm_->get_base_addr();
            auto size = dsm_->get_base_size();
            auto ctx = dsm_->get_dir_rdma_context(0);
            mr_ = createMemoryRegion(base, size, ctx);
            dsm_->put("rkey", mr_->rkey, 100ms);
        }
        rkey_ = dsm_->get<uint32_t>("rkey", 100ms);
    }
    void exit() override
    {
        if (mr_)
        {
            destroyMemoryRegion(mr_);
            mr_ = nullptr;
        }
        Base::exit();
    }
    void thread_init(BLS &bls, size_t thread_nr) override
    {
        dsm_->registerThread();
        Base::thread_init(bls, thread_nr);
    }
    void server(BLS &bls, const Config &conf, ::bench::StopToken::pointer token)
    {
        std::ignore = bls;
        auto order = conf.get<Order>("order");
        char *base = (char *) dsm_->remote_info()[server_nid_].dsmBase;
        auto tid = dsm_->get_thread_id();
        bool need_to_run = tid == 0;

        memset(base, 0, 2 * kMaxAppThread * FLAGS_cl_nr * 64);
        [[maybe_unused]] uint64_t ch = 0;

        while (!token->stop_requested())
        {
            if (need_to_run)
            {
                if (order == WW)
                {
                    volatile int64_t *pa = (int64_t *) (base);
                    volatile int64_t *pb = (int64_t *) (base + 128 - 8);
                    volatile int64_t *pc = (int64_t *) (base + 128 + 8);
                    volatile int64_t *pd = (int64_t *) (base + 256 - 8);

                    int64_t d = *pd;
                    int64_t c = *pc;
                    int64_t b = *pb;
                    int64_t a = *pa;
                    std::set<int64_t> s;
                    std::array<int64_t, 4> arr;
                    s.insert(a);
                    s.insert(b);
                    s.insert(c);
                    s.insert(d);
                    arr[0] = a;
                    arr[1] = b;
                    arr[2] = c;
                    arr[3] = d;
                    // LOG(INFO) << PRE(arr);

                    if (unlikely(a <= 0 || b <= 0 || c <= 0 || d <= 0))
                    {
                        continue;
                    }

                    if ((b - a) % 4 != 0)
                    {
                        LOG(ERROR) << PRE(arr);
                    }
                    if (c > b)
                    {
                        if (unlikely(b + 1 != c))
                        {
                            LOG(ERROR) << PRE(arr);
                        }
                    }
                    else
                    {
                        if ((b - c) % 4 != 3)
                        {
                            LOG(ERROR) << PRE(arr);
                        }
                    }
                    if ((c - d) % 4 != 0)
                    {
                        LOG(ERROR) << PRE(arr);
                    }

                    // std::this_thread::sleep_for(20ms);
                    token->complete_task(1);
                }
                else if (order == RW)
                {
                    // LOG(INFO) << "Do nothing";
                }
                else if (order == WR)
                {
                }
                else if (order == RR)
                {
                    auto *atm = (std::atomic<uint64_t> *) base;
                    for (size_t i = 0; i < 128 / sizeof(uint64_t); ++i)
                    {
                        atm[i].store(ch);
                    }
                    ch++;
                }
                else
                {
                    LOG(FATAL) << "Unknown order";
                }
            }
        }
    }
    void client(BLS &bls, const Config &conf, ::bench::StopToken::pointer token)
    {
        std::ignore = bls;
        auto order = conf.get<Order>("order");
        auto tid = dsm_->get_thread_id();

        auto buf1 = dsm_->get_rdma_buffer(4_KB);
        auto buf2 = dsm_->get_rdma_buffer(4_KB);
        uint64_t ch = 0;

        bool first = true;
        auto &tls = bls.thread();
        while (!token->stop_requested())
        {
            if (dsm_->get_node_id() == client_nid_)
            {
                if (order == WW)
                {
                    GlobalAddress gaddr1;
                    gaddr1.nodeID = server_nid_;
                    gaddr1.offset = 256 * tid;
                    GlobalAddress gaddr2;
                    gaddr2.nodeID = server_nid_;
                    gaddr2.offset = 256 * tid + 128;
                    for (size_t i = 0; i < 128 / sizeof(ch); ++i)
                    {
                        uint64_t *b1 = (uint64_t *) buf1.buffer;
                        uint64_t *b2 = (uint64_t *) buf2.buffer;
                        b1[i] = ch;
                        b2[i] = ch + 1;
                    }

                    dsm_->prepare_write(
                        buf1.buffer, gaddr1, 128, false, nullptr);
                    dsm_->prepare_write(
                        buf2.buffer, gaddr2, 128, false, nullptr);
                    dsm_->commit(nullptr);
                    token->complete_task(1);
                    // std::this_thread::sleep_for(20us);

                    ch += 4;
                }
                else if (order == RW)
                {
                    // RW order is possible
                    /**
                     * E0609 15:41:22.152418  6201 correct_order.cpp:241] expect
64, got 65 0x000000: 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
AAAAAAAAAAAAAAAA 0x000010: 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
AAAAAAAAAAAAAAAA 0x000020: 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
AAAAAAAAAAAAAAAA 0x000030: 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
AAAAAAAAAAAAAAAA
                     *
                    */
                    GlobalAddress gaddr;
                    gaddr.nodeID = server_nid_;
                    gaddr.offset = 512 * tid;

                    size_t size = 4_KB;

                    char expect_ch = ch;
                    ch++;
                    memset(buf1.buffer, 0, size);
                    memset(buf2.buffer, (char) ch, size);
                    dsm_->prepare_read(
                        buf1.buffer, gaddr, size, false, nullptr);
                    dsm_->prepare_write(
                        buf2.buffer, gaddr, size, false, nullptr);
                    dsm_->commit(nullptr);
                    token->complete_task(1);
                    if (!first)
                    {
                        bool reordered = false;
                        for (size_t i = 0; i < size; ++i)
                        {
                            if (buf1.buffer[i] != expect_ch)
                            {
                                // LOG(ERROR)
                                //     << " expect "
                                //     << util::pre_hex((int) expect_ch)
                                //     << ", got "
                                //     << util::pre_hex((int) buf1.buffer[i])
                                //     << " at " << PRE(i) << std::endl
                                //     << util::Hexdump(buf1.buffer, 512);
                                // std::this_thread::sleep_for(100ms);
                                reordered = true;
                                break;
                            }
                            // CHECK_EQ(buf1.buffer[i], expect_ch)
                            //     << " expect " << (uint64_t) expect_ch
                            //     << ", got " << (uint64_t) buf1.buffer[i]
                            //     << std::endl
                            //     << util::Hexdump(buf1.buffer, 256);
                        }
                        for (size_t i = 1; i < size; ++i)
                        {
                            if (buf1.buffer[i] != buf1.buffer[i - 1])
                            {
                                tls.rw.split_at.insert(i);
                            }
                        }
                        for (size_t i = 0; i < size; ++i)
                        {
                            CHECK(buf1.buffer[i] == expect_ch ||
                                  buf1.buffer[i] == (char) ch)
                                << "got: " << (int) buf1.buffer[i]
                                << ", expect: " << (int) expect_ch
                                << " or ch: " << (int) expect_ch;
                        }
                        if (reordered)
                        {
                            tls.rw.reorder++;
                        }
                        else
                        {
                            tls.rw.ok++;
                        }
                    }
                    first = false;
                }
                else if (order == WR)
                {
                    constexpr static size_t size = 4_KB;
                    // WR reorder will not happen
                    GlobalAddress gaddr;
                    gaddr.nodeID = server_nid_;
                    gaddr.offset = 2 * size * tid;

                    ch++;
                    char expect_ch = ch;
                    memset(buf1.buffer, (char) expect_ch, size);
                    memset(buf2.buffer, 0, size);
                    dsm_->prepare_write(
                        buf1.buffer, gaddr, size, false, nullptr);
                    dsm_->prepare_read(
                        buf2.buffer, gaddr, size, false, nullptr);
                    dsm_->commit(nullptr);
                    token->complete_task(1);
                    for (size_t i = 0; i < size; ++i)
                    {
                        // CHECK_EQ(buf2.buffer[i], expect_ch)
                        //     << " expect " << (uint64_t) expect_ch << ", got "
                        //     << (uint64_t) buf1.buffer[i] << std::endl
                        //     << util::Hexdump(buf1.buffer, size);
                        if (unlikely(buf2.buffer[i]) != expect_ch)
                        {
                            LOG(ERROR) << " expect " << (uint64_t) expect_ch
                                       << ", got " << (uint64_t) buf2.buffer[i]
                                       << std::endl
                                       << util::Hexdump(buf2.buffer, 128);
                            std::this_thread::sleep_for(200ms);
                        }
                    }
                }
                else if (order == RR)
                {
                    // cacheline inside a READ: reorder
                    //
                    /**
                     *
                     * see gD, fD and eD.
                     *
                 0x000000: 67 44 03 01 00 00 00 00 67 44 03 01 00 00 00 00
gD......gD...... 0x000010: 67 44 03 01 00 00 00 00 66 44 03 01 00 00 00 00
gD......fD...... 0x000020: 66 44 03 01 00 00 00 00 66 44 03 01 00 00 00 00
fD......fD...... 0x000030: 66 44 03 01 00 00 00 00 66 44 03 01 00 00 00 00
fD......fD...... 0x000040: 66 44 03 01 00 00 00 00 66 44 03 01 00 00 00 00
fD......fD...... 0x000050: 66 44 03 01 00 00 00 00 66 44 03 01 00 00 00 00
fD......fD...... 0x000060: 66 44 03 01 00 00 00 00 66 44 03 01 00 00 00 00
fD......fD...... 0x000070: 66 44 03 01 00 00 00 00 65 44 03 01 00 00 00 00
fD......eD......
                     *
                    */
                    // two read: reorder
                    /**
                     *
                     * the first buffer got more 27, the second buffer got
less 27.
                     * So, the second buffer reads BEFORE the first buffer.
                     *
                     *E0609 14:55:33.402745 40982 correct_order.cpp:359] v1:
17003303, v2: 17003302 0x000000: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00
's......'s...... 0x000010: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00
's......'s...... 0x000020: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00
's......'s...... 0x000030: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00
's......'s...... 0x000040: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00
's......'s...... 0x000050: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00
's......'s...... 0x000060: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00
's......'s...... 0x000070: 27 73 03 01 00 00 00 00 26 73 03 01 00 00 00 00
's......&s......

0x000000: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00  's......'s......
0x000010: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00  's......'s......
0x000020: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00  's......'s......
0x000030: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00  's......'s......
0x000040: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00  's......'s......
0x000050: 27 73 03 01 00 00 00 00 27 73 03 01 00 00 00 00  's......'s......
0x000060: 27 73 03 01 00 00 00 00 26 73 03 01 00 00 00 00  's......&s......
0x000070: 26 73 03 01 00 00 00 00 26 73 03 01 00 00 00 00  &s......&s......
                     *
                     *
                     */
                    GlobalAddress gaddr1;
                    gaddr1.nodeID = server_nid_;
                    gaddr1.offset = 0;

                    memset(buf1.buffer, 0, 128);
                    memset(buf2.buffer, 0, 128);
                    dsm_->prepare_read(
                        buf1.buffer, gaddr1, 128, false, nullptr);
                    dsm_->prepare_read(
                        buf2.buffer, gaddr1, 128, false, nullptr);
                    dsm_->commit(nullptr);
                    token->complete_task(1);

                    auto *bf1 = (int64_t *) buf1.buffer;
                    auto *bf2 = (int64_t *) buf2.buffer;
                    bool b1_changed = false;
                    bool b2_changed = false;
                    for (size_t i = 0; i < 128 / sizeof(int64_t); ++i)
                    {
                        int64_t v1 = bf1[i];
                        int64_t v2 = bf2[i];
                        if (i > 0)
                        {
                            int64_t v1_prev = bf1[i - 1];
                            int64_t v2_prev = bf2[i - 1];
                            if (v1 != v1_prev)
                            {
                                if (unlikely(b1_changed))
                                {
                                    LOG(ERROR)
                                        << PRE(v1_prev) << ", " << PRE(v1)
                                        << ", " << PRE(b1_changed) << std::endl
                                        << util::Hexdump(bf1, 128) << std::endl
                                        << util::Hexdump(bf2, 128);
                                }
                                else
                                {
                                    b1_changed = true;
                                }
                                if (v1_prev != v1 + 1)
                                {
                                    LOG(ERROR)
                                        << PRE(v1_prev) << ", " << PRE(v1)
                                        << ", " << PRE(b1_changed) << std::endl
                                        << util::Hexdump(bf1, 128) << std::endl
                                        << util::Hexdump(bf2, 128);
                                }
                            }
                            if (v2 != v2_prev)
                            {
                                if (unlikely(b2_changed))
                                {
                                    LOG(ERROR)
                                        << PRE(v2_prev) << ", " << PRE(v2)
                                        << ", " << PRE(b2_changed) << std::endl
                                        << util::Hexdump(bf1, 128) << std::endl
                                        << util::Hexdump(bf2, 128);
                                }
                                else
                                {
                                    b2_changed = true;
                                }
                                if (v2_prev != v2 + 1)
                                {
                                    LOG(ERROR)
                                        << PRE(v2_prev) << ", " << PRE(v2)
                                        << ", " << PRE(b2_changed) << std::endl
                                        << util::Hexdump(bf1, 128) << std::endl
                                        << util::Hexdump(bf2, 128);
                                }
                            }
                        }
                        if (v1 <= 0 || v2 <= 0)
                        {
                            continue;
                        }
                        if (unlikely(v1 > v2))
                        {
                            LOG(ERROR)
                                << PRE(v1) << ", " << PRE(v2) << std::endl
                                << util::Hexdump(bf1, 128) << std::endl
                                << util::Hexdump(bf2, 128);
                        }
                    }
                    // LOG(INFO) << std::endl
                    //           << util::Hexdump(bf1, 128) << std::endl
                    //           << util::Hexdump(bf2, 128);
                    // std::this_thread::sleep_for(100ms);
                }
                else
                {
                    LOG(FATAL) << "Unknown order";
                }
            }
        }
        LOG(INFO) << PRE(tls.rw);
        // LOG(INFO) << PRE(tls.rw.split_at);
        auto min_not_dividable = 512;
        for (size_t gcd : {8, 16, 32, 64, 128, 256, 512})
        {
            for (auto split : tls.rw.split_at)
            {
                if (split % gcd != 0)
                {
                    min_not_dividable = std::min(min_not_dividable, (int) gcd);
                }
            }
        }
        LOG(ERROR) << "Not dividable by " << PRE(min_not_dividable);
    }
    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        if (dsm_->get_node_id() == server_nid_)
        {
            server(bls, conf, token);
        }
        else
        {
            client(bls, conf, token);
        }
    }

private:
    DSM::pointer dsm_;
    size_t server_nid_{1};
    size_t client_nid_{0};
    uint32_t rkey_;
    ibv_mr *mr_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    f.configure_thread_nr({8});
    // f.add_option<Order>("order", {RW, WR});
    // f.add_option<Order>("order", {WR});
    // f.add_option<Order>("order", {RW});
    f.add_option<Order>("order", {RW});
    // f.add_option<Order>("order", {RR});
    auto configs = f.generate_configs();

    exp.configure_monitor(500ms, 10);
    exp.launch(configs);

    LOG(INFO) << "PASS.";
}