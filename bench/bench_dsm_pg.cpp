#include <numa.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <thread>

#include "Common.h"
#include "DSMCache.h"
#include "WRLock.h"
#include "bench/DataFrame.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "memory/dsm_allocator.h"
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

    Buffer get_rdma_buffer(BLS &bls, const Config &conf, size_t size)
    {
        auto tid = bls.id().thread_id;
        auto cid = bls.id().coro_id;
        [[maybe_unused]] auto uid = tid * conf.coro_nr() + cid;
        [[maybe_unused]] auto max_uid = conf.thread_nr() * conf.coro_nr();

        char *base = (char *) dsm_->get_cache().data;

        auto aligned_size = util::round_up_aligned(size, 64);
        // CHECK_EQ(aligned_size, 64) << "TODO...: playing around that";
        // auto aligned_size = size;

        [[maybe_unused]] auto total_size_thread = aligned_size * conf.coro_nr();
        [[maybe_unused]] auto total_size_coro = aligned_size * conf.thread_nr();

        // char *start = base + tid * total_size_thread;
        // char *start = base + cid * total_size_coro;
        char *start = base + aligned_size * uid;
        // char *start = base + 2_MB * uid;
        // char *start = base;
        // return Buffer(start, aligned_size);
        // return Buffer(base, size);
        // char *start = base + tid * total_size_thread + cid * aligned_size;
        return Buffer(start, aligned_size);
    }

    GlobalAddress get_gaddr(GlobalAddress base_gaddr,
                            [[maybe_unused]] BLS &bls,
                            [[maybe_unused]] const Config &conf,
                            [[maybe_unused]] uint64_t addr_rng,
                            [[maybe_unused]] size_t io_size)
    {
        // uint64_t offset =
        //     fast_pseudo_rand_int(0, (addr_rng - 1) / io_size) * io_size;
        // uint64_t offset = 0;
        [[maybe_unused]] auto tid = bls.id().thread_id;
        [[maybe_unused]] auto cid = bls.id().coro_id;
        [[maybe_unused]] auto uid = tid * conf.coro_nr() + cid;
        [[maybe_unused]] auto aligned_io_size =
            util::round_up_aligned(io_size, 64);
        uint64_t offset = uid * aligned_io_size;
        // uint64_t offset = uid * 4_KB;
        // uint64_t offset = 0;
        // LOG(INFO) << PRE(uid, (void *) offset);
        return base_gaddr + offset;
    }
    char *get_addr(char *base_addr, uint64_t addr_rng, size_t io_size)
    {
        uint64_t offset =
            fast_pseudo_rand_int(0, (addr_rng - 1) / io_size) * io_size;
        return base_addr + offset;
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
        // auto rdma_buffer = dsm->get_rdma_buffer(4_KB);
        auto rdma_buffer = get_rdma_buffer(bls, conf, io_size);

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
                void *addr = get_addr((char *) buf_addr, addr_rng, io_size);
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
                    // uint64_t offset =
                    //     fast_pseudo_rand_int(0, (addr_rng - 1) / io_size) *
                    //     io_size;
                    // auto addr = gaddr + offset;
                    auto addr = get_gaddr(gaddr, bls, conf, addr_rng, io_size);
                    if (op == Op::kRead)
                    {
                        dsm->prepare_read(
                            rdma_buffer.buffer, addr, io_size, on_chip, &ctx);
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
                dsm->commit(&ctx);
                token->complete_task(batch_size);
                tls.op += batch_size;
            }
        }
        dsm->put_rdma_buffer(std::move(rdma_buffer));
    }

private:
    DSM::pointer dsm_;
    std::vector<void *> huge_pages_;

    bench::ResultDataFrame df_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    // f.configure_thread_nr({1, 2, 4, 8, 12, 16, 20, 24, 28, 32,
    // kMaxAppThread});
    f.configure_thread_nr({kMaxAppThread});
    // f.configure_thread_nr({1});
    f.configure_coro_nr({3});
    f.add_option<size_t>("addr_rng", {2_GB});
    // f.add_option<size_t>("addr_rng", {define::kLockChipMemSize});
    // f.add_option<size_t>("addr_rng", {32});
    // f.add_option<size_t>("io_size", {8, 32});
    f.add_option<size_t>("io_size", {32});
    f.add_option<size_t>("batch_size", {1});
    // f.add_option<enum Op>("op", {Op::kWrite});
    // f.add_option<enum Op>("op", {Op::kRead, Op::kWrite});
    // f.add_option<enum Op>("op", {Op::kCAS, Op::kFAA});
    f.add_option<enum Op>("op", {Op::kRead, Op::kWrite, Op::kCAS, Op::kFAA});
    // f.add_option<enum Op>("op", {Op::kCAS});
    // f.add_option<enum Location>("location",
    //                             {Location::kRemoteDRAM,
    //                              Location::kRemoteRNIC,
    //                              Location::kLocalDRAM,
    //                              Location::kLocalRNIC});
    f.add_option<enum Location>("location", {Location::kRemoteRNIC});
    // f.add_option<enum Location>("location", {Location::kCPU});
    auto configs = f.generate_configs();

    exp.configure_monitor(100ms, 6);
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}