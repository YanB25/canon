#include <numa.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <thread>

#include "Common.h"
#include "DSMCache.h"
#include "PerThread.h"
#include "WRLock.h"
#include "avis/ptl.h"
#include "bench/DataFrame.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
#include "util/Rand.h"
#include "util/gflags_def.h"

struct Spec
{
    struct TLS
    {
        size_t op{};
    };
};

enum class Op
{
    kWrite,
    kRead,
    kFAA,
    kCAS,
};
std::ostream &operator<<(std::ostream &os, Op op)
{
    switch (op)
    {
    case Op::kWrite:
        os << "kWrite";
        break;
    case Op::kRead:
        os << "kRead";
        break;
    case Op::kFAA:
        os << "kFAA";
        break;
    case Op::kCAS:
        os << "kCAS";
        break;
    }
    return os;
}

enum class Location
{
    kLocalDRAM,
    kLocalRNIC,
    kRemoteDRAM,
    kRemoteRNIC,
    kCPU,
};

std::ostream &operator<<(std::ostream &os, Location l)
{
    switch (l)
    {
    case Location::kLocalDRAM:
        os << "kLocalDRAM";
        break;
    case Location::kLocalRNIC:
        os << "kLocalRNIC";
        break;
    case Location::kRemoteDRAM:
        os << "kRemoteDRAM";
        break;
    case Location::kRemoteRNIC:
        os << "kRemoteRNIC";
        break;
    case Location::kCPU:
        os << "kCPU";
    }
    return os;
}

class Experiment : public ::bench::DSMCoroExperiment<Spec>
{
public:
    using Base = ::bench::DSMCoroExperiment<Spec>;
    using BLS = typename Base::BLS;
    constexpr static char kCh = 0;
    Experiment()
    {
        DSMConfig config;
        config.worker_nr = 0;
        dsm_ = DSM::getInstance(config);
        dsm_->registerThread();
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
        // LOG(INFO) << "Initing rm to all " << util::pre(kCh);
        // dsm->prepare_write(rdma_buffer.buffer, addr, io_size, on_chip, &ctx);

        char *base_addr = (char *) dsm_->get_base_addr();
        auto addr_rng = conf.get<size_t>("addr_rng");
        memset(base_addr, kCh, addr_rng);

        Base::on_start_bench(bls, conf);
    }
    void on_end_bench(const ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        huge_pages_.clear();
        auto io_size = conf.get<size_t>("io_size");
        uint64_t throughput = res.cluster_ops * io_size;
        LOG(INFO) << "result: " << util::pre_bw(throughput) << "("
                  << util::pre_bit_bw(throughput) << ")";

        df_.reg_result(res, conf);

        Base::on_end_bench(res, bls, conf);
    }

    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        // LOG(INFO) << "Tid " << util::get_thread_id() << ": " <<
        // bls.thread().op;
        Base::on_thread_end_bench(bls, conf);
    }

    void exit() override
    {
        df_.dump(FLAGS_binary, FLAGS_exec_meta);
    }

    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool master) override
    {
        auto &tls = bls.thread();
        auto dsm = get_dsm();
        auto io_size = conf.get<size_t>("io_size");
        auto batch_size = conf.get<size_t>("batch_size");
        auto op = conf.get<enum Op>("op");
        auto addr_rng = conf.get<size_t>("addr_rng");
        auto loc = conf.get<enum Location>("location");

        auto nid = dsm_->get_node_id();
        auto next_nid = (nid + 1) % dsm_->getClusterSize();
        GlobalAddress gaddr;
        gaddr.offset = 0;
        bool on_chip;
        if (loc == Location::kLocalDRAM)
        {
            gaddr.nodeID = nid;
            on_chip = false;
        }
        else if (loc == Location::kLocalRNIC)
        {
            gaddr.nodeID = nid;
            on_chip = true;
        }
        else if (loc == Location::kRemoteDRAM)
        {
            gaddr.nodeID = next_nid;
            on_chip = false;
        }
        else if (loc == Location::kRemoteRNIC)
        {
            gaddr.nodeID = next_nid;
            on_chip = true;
        }
        else if (loc == Location::kCPU)
        {
            gaddr.nodeID = nid;
        }

        if (loc == Location::kRemoteRNIC || loc == Location::kLocalRNIC)
        {
            // device memory is limited.
            addr_rng = std::min(addr_rng, define::kLockChipMemSize);
        }

        if (op == Op::kCAS || op == Op::kFAA)
        {
            if (unlikely(io_size > 32))
            {
                LOG_IF(INFO, master) << "** not supported. Skip";
                token->core().request_stop();
            }
        }
        [[maybe_unused]] auto thread_nr = conf.thread_nr();
        [[maybe_unused]] auto tid = util::get_thread_id();
        // auto id = ctx.coro_id() + tid * define::kMaxCoroNr;
        auto rdma_buffer = dsm->get_rdma_buffer(std::max(4_KB, io_size));
        rdma_buffer.buffer += 8 * fast_pseudo_rand_int(0, 32);

        uint64_t compare = (uint64_t) malloc(io_size);
        uint64_t compare_mask = (uint64_t) malloc(io_size);
        uint64_t swap = (uint64_t) malloc(io_size);
        uint64_t swap_mask = (uint64_t) malloc(io_size);
        uint64_t inl_compare = 0;
        uint64_t inl_compare_mask = 0xffffffffffffffff;
        uint64_t inl_swap = 1;
        uint64_t inl_swap_mask = 0xffffffffffffffff;
        uint64_t add_val = (uint64_t) malloc(io_size);
        *(uint64_t *) add_val = 1;
        uint64_t inl_add_val = 1;
        uint64_t field_boundary = (uint64_t) malloc(io_size);
        uint64_t inl_field_boundary = 1ull << 16;
        *(uint64_t *) field_boundary = inl_field_boundary;

        CHECK_GE(addr_rng, io_size);

        auto [buf_nid, buf_addr] = dsm_->explain_gaddr(gaddr);
        char *result_buffer = (char *) malloc(io_size);
        while (!token->stop_requested())
        {
            if (unlikely(loc == Location::kCPU))
            {
                uint64_t offset =
                    fast_pseudo_rand_int(0, (addr_rng - 1) / io_size) * io_size;
                void *addr = (char *) buf_addr + offset;
                if (op == Op::kRead)
                {
                    memcpy(result_buffer, addr, io_size);
                }
                else if (op == Op::kWrite)
                {
                    memcpy(addr, result_buffer, io_size);
                }
                else if (op == Op::kCAS)
                {
                    auto *atm = (std::atomic<uint64_t> *) addr;
                    uint64_t expect = 0;
                    atm->compare_exchange_strong(expect, 1);
                }
                else if (op == Op::kFAA)
                {
                    auto *atm = (std::atomic<uint64_t> *) addr;
                    atm->fetch_add(1);
                }
                else
                {
                    LOG(FATAL) << "Unknown op " << PRE((int) op);
                }
                token->complete_task(1);
            }
            else
            {
                for (size_t i = 0; i < batch_size; ++i)
                {
                    uint64_t offset =
                        fast_pseudo_rand_int(0, (addr_rng - 1) / io_size) *
                        io_size;
                    auto addr = gaddr + offset;
                    if (op == Op::kRead)
                    {
                        // LOG(INFO) << "Issue prepare_read.";
                        dsm->prepare_read(
                            rdma_buffer.buffer, addr, io_size, on_chip, &ctx);
                        rdma_buffer.buffer[io_size - 1] = 0xff;
                        // waiting_coro_.current().push_back(
                        //     {.buf = &rdma_buffer.buffer[io_size - 1],
                        //      .expect = kCh,
                        //      .who = ctx.coro_id()});
                        auto &slot = waiting_coro_.current()[ctx.coro_id()];
                        CHECK(!slot.waiting) << "Should be not waiting";
                        slot.waiting = true;
                        slot.buf = &rdma_buffer.buffer[io_size - 1];
                        slot.expect = kCh;
                    }
                    else if (op == Op::kWrite)
                    {
                        dsm->prepare_write(
                            rdma_buffer.buffer, addr, io_size, on_chip, &ctx);
                    }
                    else if (op == Op::kCAS)
                    {
                        dsm->prepare_cas(
                            addr,
                            io_size,
                            io_size > 8 ? compare : inl_compare,
                            io_size > 8 ? compare_mask : inl_compare_mask,
                            io_size > 8 ? swap : inl_swap,
                            io_size > 8 ? swap_mask : inl_swap_mask,
                            (uint64_t *) rdma_buffer.buffer,
                            on_chip,
                            &ctx);
                    }
                    else if (op == Op::kFAA)
                    {
                        // dsm->prepare_faa(
                        //     addr, 1, (uint64_t *) rdma_buffer.buffer, &ctx);
                        dsm->prepare_faa(
                            addr,
                            io_size,
                            io_size > 8 ? add_val : inl_add_val,
                            io_size > 8 ? field_boundary : inl_field_boundary,
                            rdma_buffer.buffer,
                            on_chip,
                            &ctx);
                    }
                    else
                    {
                        LOG(FATAL) << "Unknown op " << PRE((int) op);
                    }
                }
                thread_local static int cnt = 0;
                if (cnt++ % 2 == 0)
                {
                    dsm->commit(&ctx);
                }
                else
                {
                    dsm->commit_no_wait(&ctx);
                    ctx.yield_to_master();
                }

                token->complete_task(batch_size);

                tls.op += batch_size;
            }
        }
        dsm->put_rdma_buffer(std::move(rdma_buffer));
    }

    void benchmark_master(StorageT &,
                          const Config &conf,
                          CoroContext &mctx,
                          ::bench::StopToken::pointer token,
                          bool) override
    {
        size_t coro_nr = conf.coro_nr();
        size_t total_coro_nr = conf.effective_client_nr();

        finished_coro_.current().clear();
        finished_coro_.current().resize(coro_nr, false);
        finished_nr_.current() = 0;

        [[maybe_unused]] auto &_dsm = this->__dsm();

        for (size_t i = 0; i < coro_nr; ++i)
        {
            mctx.yield_to_worker(i);
        }

        while (total_finished_nr_.load(std::memory_order_relaxed) <
               total_coro_nr)
        {
            _dsm.try_master_coro_poll(&mctx);
            // asm volatile("" ::: "memory");
            // bool has_found = false;
            // constexpr static auto kSleepTime = 100ns;
            for (coro_t cid = 0; cid < coro_nr; cid++)
            {
                // asm volatile("" ::: "memory");
                auto &slot = waiting_coro_.current()[cid];
                if (slot.waiting)
                {
                    if (*slot.buf == slot.expect)
                    {
                        slot.waiting = false;
                        mctx.yield_to_worker(cid);
                        // has_found = true;
                    }
                }
            }
            // if (!has_found)
            // {
            // auto then = std::chrono::steady_clock::now();
            // while (std::chrono::steady_clock::now() - then < kSleepTime)
            // {
            //     util::asms::cpu_relax();
            // }
            // }
        }

        CHECK(token->stop_requested());
    }

private:
    DSM::pointer dsm_;
    std::vector<void *> huge_pages_;

    struct Poll
    {
        volatile char *buf{};
        char expect{};
        bool waiting{false};
    };
    Perthread<std::array<Poll, 32>> waiting_coro_{};

    bench::ResultDataFrame df_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    // f.configure_thread_nr({1, 2, 4, 8, 12, 16, 20, 24, 28, 32,
    // kMaxAppThread});
    // f.configure_thread_nr({16, kMaxAppThread});
    f.configure_thread_nr({kMaxAppThread});
    // f.configure_thread_nr({24});
    f.configure_coro_nr({6});
    // f.configure_coro_nr({8});
    // f.add_option<size_t>("addr_rng", {4_KB, 2_MB, 1_GB, 8_GB});
    // f.add_option<size_t>("addr_rng", {1_GB});
    f.add_option<size_t>("addr_rng", {32_MB});
    f.add_option<size_t>("io_size", {8});
    f.add_option<size_t>("batch_size", {1});
    // f.add_option<enum Op>("op", {Op::kWrite});
    // f.add_option<enum Op>("op", {Op::kRead, Op::kWrite});
    f.add_option<enum Op>("op", {Op::kRead});
    // f.add_option<enum Op>("op", {Op::kCAS, Op::kFAA});
    // f.add_option<enum Op>("op", {Op::kRead, Op::kWrite, Op::kCAS, Op::kFAA});
    // f.add_option<enum Op>("op", {Op::kCAS});
    // f.add_option<enum Location>("location",
    //                             {Location::kRemoteDRAM,
    //                              Location::kRemoteRNIC,
    //                              Location::kLocalDRAM,
    //                              Location::kLocalRNIC});
    f.add_option<enum Location>("location", {Location::kRemoteDRAM});
    // f.add_option<enum Location>("location", {Location::kCPU});
    auto configs = f.generate_configs();

    exp.configure_monitor(100ms, 20);
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}