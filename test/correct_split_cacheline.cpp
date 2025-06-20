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
#include "util/stacktrace.h"

struct ThreadContext
{
    GlobalAddress gaddr;
};
struct Spec
{
    using PTLS = ThreadContext;
    struct TLS
    {
        size_t op{};
    };
};

struct PrvCoroCtx
{
    Buffer lock_buf;
    Buffer unlock_buf;
    Buffer write_buf;
    Buffer read_buf;
};

class CrcObj
{
public:
    CrcObj(GlobalAddress gaddr, size_t obj_size)
        : gaddr_(gaddr), obj_size_(obj_size)
    {
    }
    std::pair<GlobalAddress, size_t> object() const
    {
        return {gaddr_, obj_size_};
    }
    std::pair<GlobalAddress, size_t> CRC() const
    {
        return {gaddr_ + obj_size_, sizeof(uint64_t)};
    }
    std::pair<GlobalAddress, size_t> lock() const
    {
        return {gaddr_ + obj_size_ + sizeof(uint64_t), sizeof(uint64_t)};
    }
    size_t size() const
    {
        return obj_size_ + 2 * sizeof(uint64_t);
    }

private:
    GlobalAddress gaddr_;
    size_t obj_size_;

} __attribute__((packed));

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
    DSM::pointer get_dsm() const override
    {
        return dsm_;
    }

    static size_t actual_data_size(size_t data_size)
    {
        auto size = data_size + 2 * sizeof(uint64_t);
        return util::round_up_aligned(size, 4_KB);
    }

    void on_start_bench(BLS &bls, const Config &conf) override
    {
        int nid = dsm_->get_node_id();
        int server_id = dsm_->getClusterSize() - 1;
        auto data_nr = conf.get<size_t>("data_nr");
        auto data_size = conf.get<size_t>("data_size");
        if (nid == server_id)
        {
            auto meta_gaddr = dsm_->alloc_from(data_nr * 8, nid);
            dsm_->put("meta", meta_gaddr, 100ms);
            auto data_gaddr =
                dsm_->alloc_from(data_nr * actual_data_size(data_size), nid);
            dsm_->put("data", data_gaddr, 100ms);
        }
        meta_ = dsm_->get<GlobalAddress>("meta", 100ms);
        data_ = dsm_->get<GlobalAddress>("data", 100ms);

        write_ok_.fill(0);
        write_nr_.fill(0);
        read_ok_.fill(0);
        read_nr_.fill(0);
        Base::on_start_bench(bls, conf);
    }
    void on_end_bench(const ResultRecord &r,
                      BLS &bls,
                      const Config &conf) override
    {
        LOG(INFO) << PRE(write_nr_);
        LOG(INFO) << PRE(write_ok_);
        LOG(INFO) << PRE(read_nr_);
        LOG(INFO) << PRE(read_ok_);
        Base::on_end_bench(r, bls, conf);
    }
    void run_ip(BLS &,
                const Config &conf,
                CoroContext &ctx,
                ::bench::StopToken::pointer token,
                size_t data_nr,
                size_t data_size,
                bool w)
    {
        std::ignore = conf;
        PrvCoroCtx prv;
        prv.lock_buf = dsm_->get_rdma_buffer(sizeof(uint64_t));
        prv.unlock_buf = dsm_->get_rdma_buffer(sizeof(uint64_t));
        prv.write_buf = dsm_->get_rdma_buffer(actual_data_size(data_size));
        prv.read_buf = dsm_->get_rdma_buffer(actual_data_size(data_size));

        while (!token->stop_requested())
        {
            auto ith = fast_pseudo_rand_int(0, data_nr - 1);
            if (w)
            {
                if (try_lock(ith, ctx, prv))
                {
                    write_data(ith, data_size, ctx, prv);
                    write_ok_.current()++;
                    unlock(ith, ctx, prv);
                    token->complete_task(1);
                }
                else
                {
                    // lock fail, do nothing
                }
                write_nr_.current()++;
            }
            else
            {
                auto ith = fast_pseudo_rand_int(0, data_nr - 1);
                if (read_data_consistency(ith, data_size, &ctx, prv))
                {
                    read_ok_.current()++;
                    token->complete_task(1);
                }
                read_nr_.current()++;
            }
        }
    }
    bool try_lock(size_t ith, CoroContext &ctx, const PrvCoroCtx &prv)
    {
        GlobalAddress lock_gaddr;
        lock_gaddr.nodeID = meta_.nodeID;
        lock_gaddr.offset = ith * sizeof(uint64_t);
        bool ret = dsm_->cas_dm_sync(
            lock_gaddr, 0, 1, (uint64_t *) prv.lock_buf.buffer, &ctx);
        return ret;
    }
    void unlock(size_t ith, CoroContext &ctx, const PrvCoroCtx &prv)
    {
        GlobalAddress lock_gaddr;
        lock_gaddr.nodeID = meta_.nodeID;
        lock_gaddr.offset = ith * sizeof(uint64_t);
        memset(prv.unlock_buf.buffer, 0, sizeof(uint64_t));
        dsm_->write_dm(
            prv.unlock_buf.buffer, lock_gaddr, sizeof(uint64_t), false, &ctx);
    }
    bool check_consistency(char *buf, size_t size, bool debug)
    {
        auto *pfirst = (std::atomic<uint64_t> *) buf;
        uint64_t first = pfirst->load();
        auto *plast =
            (std::atomic<uint64_t> *) ((char *) buf + sizeof(uint64_t) + size);
        uint64_t last = plast->load();
        if (first == last)
        {
            auto *pexpect = (std::atomic<char> *) (buf + sizeof(uint64_t));
            char expect = pexpect->load();
            for (size_t i = 0; i < size; ++i)
            {
                // char *ptr = buf + sizeof(uint64_t) + i;
                auto *p = (std::atomic<char> *) (buf + sizeof(uint64_t) + i);
                char read = p->load();
                CHECK_EQ(read, expect)
                    << "[debug: " << debug << " ]"
                    << "** mismatch at " << PRE(i) << std::endl
                    << util::Hexdump(buf, size + 2 * sizeof(uint64_t));
            }
        }
        return first == last;
    }
    bool read_data_consistency(size_t ith,
                               size_t data_size,
                               CoroContext *ctx,
                               const PrvCoroCtx &prv)
    {
        auto ith_data_gaddr = data_ + ith * actual_data_size(data_size);
        dsm_->read_sync(prv.read_buf.buffer,
                        ith_data_gaddr,
                        data_size + 2 * sizeof(uint64_t),
                        ctx);
        // LOG_EVERY_N(INFO, 1000)
        //     << std::endl
        //     << util::Hexdump(prv.read_buf.buffer,
        //                      data_size + 2 * sizeof(uint64_t));
        return check_consistency(prv.read_buf.buffer, data_size, false);
    }
    void write_data(size_t ith,
                    size_t data_size,
                    CoroContext &ctx,
                    const PrvCoroCtx &prv)
    {
        uint64_t magic_version = fast_pseudo_rand_int();
        char *buf = prv.write_buf.buffer;
        char magic_ch = fast_pseudo_rand_printable_char();
        memcpy(buf, &magic_version, sizeof(magic_version));
        memset(buf + sizeof(uint64_t), magic_ch, data_size);
        memcpy(buf + sizeof(uint64_t) + data_size,
               &magic_version,
               sizeof(magic_version));

        // TODO:
        CHECK(check_consistency(buf, data_size, true))
            << util::Hexdump(buf, 2 * sizeof(uint64_t) + data_size);

        auto ith_data_gaddr = data_ + ith * actual_data_size(data_size);
        dsm_->write_sync(prv.write_buf.buffer,
                         ith_data_gaddr,
                         data_size + 2 * sizeof(uint64_t),
                         &ctx);
    }
    void run_cow(BLS &bls,
                 const Config &conf,
                 CoroContext &ctx,
                 ::bench::StopToken::pointer token,
                 size_t data_nr,
                 size_t data_size,
                 bool w)
    {
        LOG(FATAL) << "TODO:";
        std::ignore = bls;
        std::ignore = conf;
        std::ignore = ctx;
        std::ignore = token;
        std::ignore = data_nr;
        std::ignore = data_size;
        std::ignore = w;
        // while (!token->stop_requested())
        // {
        //     if (w)
        //     {
        //     }
        // }
    }

    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        auto dsm = get_dsm();
        bool cow = conf.get<bool>("cow");
        size_t write_ratio = conf.get<size_t>("write_ratio");
        size_t thread_nr = conf.thread_nr();
        size_t writer_nr = thread_nr * write_ratio / 100.0;
        size_t data_nr = conf.get<size_t>("data_nr");
        size_t data_size = conf.get<size_t>("data_size");
        auto tid = util::get_thread_id();
        bool is_writer = tid < writer_nr;
        if (cow)
        {
            run_cow(bls, conf, ctx, token, data_nr, data_size, is_writer);
        }
        else
        {
            run_ip(bls, conf, ctx, token, data_nr, data_size, is_writer);
        }
    }

private:
    DSM::pointer dsm_;
    GlobalAddress meta_;
    GlobalAddress data_;
    Perthread<size_t> write_ok_;
    Perthread<size_t> write_nr_;
    Perthread<size_t> read_ok_;
    Perthread<size_t> read_nr_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);
    Experiment exp;

    bench::ConfigFactory f;
    // f.configure_thread_nr({1, 4, 8, 16});
    // f.configure_thread_nr({kMaxAppThread});
    f.configure_thread_nr({2});
    f.configure_coro_nr({4});
    f.add_option<size_t>("data_size", {128});
    f.add_option<size_t>("data_nr", {1});
    f.add_option<bool>("cow", {false});
    // f.add_option<size_t>("write_ratio", {0, 50, 100});
    f.add_option<size_t>("write_ratio", {50});
    auto configs = f.generate_configs();

    exp.configure_monitor(400ms, 8);
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}