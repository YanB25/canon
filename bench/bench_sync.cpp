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
#include "util/ZipRand.h"
#include "util/gflags_def.h"
#include "util/stacktrace.h"

DEFINE_double(z, 0.99, "The skewness.");
DEFINE_uint64(data_nr, 1024, "Number of data.");

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
    struct CLS
    {
        std::array<GlobalAddress, 64> gaddrs;
        size_t cur_idx{0};
    };
};

struct PrvCoroCtx
{
    Buffer lock_buf;
    Buffer unlock_buf;
    Buffer write_buf;
    Buffer read_buf;
    Buffer cas_buf;
};

class CVObj
{
public:
    using v_t = uint8_t;
    CVObj(size_t obj_size) : obj_size_(obj_size)
    {
    }
    size_t object_size()
    {
        return cl_nr() * 64;
    }
    size_t cl_nr()
    {
        auto cl = cl_data_size();
        auto cl_nr = (obj_size_ + (cl - 1)) / cl;
        return cl_nr;
    }
    // void fill_local_buf(char *buf, char ch, v_t v)
    // {
    //     ssize_t remain = obj_size_;
    //     char *ptr = buf;
    //     while (remain > 0)
    //     {
    //         memcpy(ptr, &v, sizeof(v));
    //         auto fill_size = std::min(remain, cl_data_size());
    //         memset(ptr + sizeof(v), ch, fill_size);
    //         memcpy(ptr + sizeof(v) + fill_size, &v, sizeof(v));

    //         remain -= fill_size;
    //         ptr += 64;
    //     }
    // }
    void fill_local_buf(char *buf, char ch)
    {
        auto sz = cal_total_size_nolock(obj_size_);
        memset(buf, ch, sz);
        uint64_t lock_val = 0;
        memcpy(buf + sz, &lock_val, sizeof(lock_val));
    }

    void get_obj(const char *from_buf, char *to_buf)
    {
        ssize_t remain = obj_size_;
        const char *local_ptr = from_buf;
        while (remain > 0)
        {
            auto fill_size = std::min(remain, (ssize_t) cl_data_size());
            memcpy(to_buf, local_ptr + sizeof(v_t), fill_size);

            to_buf += fill_size;
            local_ptr += 64;
            remain -= fill_size;
        }
    }
    GlobalAddress lock_gaddr(GlobalAddress gaddr) const
    {
        return gaddr + cal_total_size_nolock(obj_size_);
    }

    static size_t cl_data_size()
    {
        return 64 - sizeof(v_t);
    }

    static size_t cal_object_occupy_size(size_t obj_size)
    {
        return (obj_size / cl_data_size()) * 64 + obj_size % cl_data_size();
    }
    static size_t cal_total_size_nolock(size_t obj_size)
    {
        auto sz = cal_object_occupy_size(obj_size);
        return util::round_up_aligned(sz, 8);
    }
    size_t total_size() const
    {
        return cal_total_size(obj_size_);
    }
    static size_t cal_total_size(size_t obj_size)
    {
        return cal_total_size_nolock(obj_size) + sizeof(uint64_t);
    }

private:
    size_t obj_size_;
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
        return cal_size(obj_size_);
    }
    static size_t cal_size(size_t obj_size)
    {
        return obj_size + 2 * sizeof(uint64_t);
    }

private:
    GlobalAddress gaddr_;
    size_t obj_size_;
};

enum Sync
{
    kCRC,
    kCOW,
    kCacheline,
};

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

    static size_t actual_data_size(size_t data_size, enum Sync s)
    {
        if (s == kCRC)
        {
            return CrcObj::cal_size(data_size);
        }
        else if (s == kCOW)
        {
            return data_size;
        }
        else if (s == kCacheline)
        {
            return CVObj::cal_total_size(data_size);
        }
        else
        {
            LOG(FATAL) << "??";
        }
    }

    void on_start_bench(BLS &bls, const Config &conf) override
    {
        static size_t i = 0;

        int nid = dsm_->get_node_id();
        int server_id = dsm_->getClusterSize() - 1;
        auto data_nr = conf.get<size_t>("data_nr");
        auto data_size = conf.get<size_t>("data_size");
        auto sync_type = conf.get<enum Sync>("sync");
        if (nid == server_id)
        {
            auto meta_size = sizeof(GlobalAddress) * data_nr;
            auto meta_gaddr = dsm_->alloc_from(meta_size, nid);
            dsm_->put("meta", meta_gaddr, 100ms);
            auto [nid__, meta_buf] = dsm_->explain_gaddr(meta_gaddr);
            memset(meta_buf, 0, meta_size);
            auto total_data_size =
                data_nr * actual_data_size(data_size, sync_type);
            auto data_gaddr = dsm_->alloc_from(total_data_size, nid);
            dsm_->put("data", data_gaddr, 100ms);

            auto [nid_, buf] = dsm_->explain_gaddr(data_gaddr);
            std::ignore = nid_;
            memset(buf, 0, total_data_size);
        }
        dsm_->keeper_barrier(std::to_string(i++), 100ms);
        meta_ = dsm_->get<GlobalAddress>("meta", 100ms);
        data_ = dsm_->get<GlobalAddress>("data", 100ms);

        write_ok_.fill(0);
        write_nr_.fill(0);
        read_ok_.fill(0);
        read_nr_.fill(0);

        Base::on_start_bench(bls, conf);
    }
    void on_thread_start_bench(BLS &bls, const Config &conf) override
    {
        auto data_nr = conf.get<size_t>("data_nr");
        // init generator
        std::unique_ptr<util::Generator> base_g;
        if (FLAGS_z == 0)
        {
            base_g = std::make_unique<util::UniformGenerator>(0, data_nr - 1);
        }
        else
        {
            base_g = std::make_unique<util::ZipfianGenerator>(
                0, data_nr - 1, FLAGS_z);
        }
        g_.current() = std::make_unique<util::ShuffleGenerator>(
            std::move(base_g), data_nr);
        Base::on_thread_start_bench(bls, conf);
    }
    void on_coro_start_bench(BLS &bls, const Config &conf) override
    {
        auto sync = conf.get<enum Sync>("sync");
        if (sync == kCOW)
        {
            int server_id = dsm_->getClusterSize() - 1;
            auto &cls = bls.coroutine();
            auto data_size = conf.get<size_t>("data_size");
            for (size_t i = 0; i < cls.gaddrs.size(); ++i)
            {
                cls.gaddrs[i] = dsm_->alloc_from(data_size, server_id);
            }
            // LOG(INFO) << PRE(cls.gaddrs);
        }
        Base::on_coro_start_bench(bls, conf);
    }
    void on_coro_end_bench(BLS &bls, const Config &conf) override
    {
        auto sync = conf.get<enum Sync>("sync");
        if (sync == kCOW)
        {
            auto &cls = bls.coroutine();
            auto data_size = conf.get<size_t>("data_size");
            for (auto gaddr : cls.gaddrs)
            {
                dsm_->free(gaddr, data_size);
            }
        }
        Base::on_coro_end_bench(bls, conf);
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
    void run_cow(BLS &bls,
                 const Config &conf,
                 CoroContext &ctx,
                 ::bench::StopToken::pointer token,
                 size_t data_nr,
                 size_t data_size,
                 bool w)
    {
        std::ignore = conf;
        PrvCoroCtx prv;

        auto sync_type = conf.get<enum Sync>("sync");
        auto total_size = actual_data_size(data_size, sync_type);

        prv.lock_buf = dsm_->get_rdma_buffer(sizeof(uint64_t));
        prv.cas_buf = dsm_->get_rdma_buffer(sizeof(uint64_t));
        prv.unlock_buf = dsm_->get_rdma_buffer(sizeof(uint64_t));
        prv.write_buf = dsm_->get_rdma_buffer(total_size);
        prv.read_buf = dsm_->get_rdma_buffer(total_size);

        bool report_writer = conf.get<bool>("report_writer");

        auto &cls = bls.coroutine();

        while (!token->stop_requested())
        {
            auto ith = g_.current()->Next();
            DCHECK_LT(ith, data_nr);
            auto ith_meta_gaddr = meta_ + sizeof(GlobalAddress) * ith;

            if (w)
            {
                auto cur_data_gaddr = cls.gaddrs[cls.cur_idx];
                cls.cur_idx = (cls.cur_idx + 1) % cls.gaddrs.size();

                char *buf = prv.write_buf.buffer;
                char magic_ch = fast_pseudo_rand_printable_char();
                memset(buf, magic_ch, data_size);
                dsm_->write_sync(buf, cur_data_gaddr, data_size, &ctx);

                uint64_t *cas_buf = (uint64_t *) prv.cas_buf.buffer;
                uint64_t expect = *(uint64_t *) cas_buf;
                while (true)
                {
                    bool succ = dsm_->cas_sync(ith_meta_gaddr,
                                               expect,
                                               cur_data_gaddr.val,
                                               cas_buf,
                                               &ctx);
                    write_nr_.current()++;
                    expect = *(uint64_t *) cas_buf;
                    if (succ)
                    {
                        if (report_writer)
                        {
                            token->complete_task(1);
                        }
                        write_ok_.current()++;
                        break;
                    }
                }
            }
            else
            {
                uint64_t *cas_buf = (uint64_t *) prv.cas_buf.buffer;
                dsm_->read_sync((char *) cas_buf,
                                ith_meta_gaddr,
                                sizeof(GlobalAddress),
                                &ctx);
                GlobalAddress data_gaddr = *(GlobalAddress *) cas_buf;
                char *buf = prv.read_buf.buffer;
                dsm_->read_sync(buf, data_gaddr, data_size, &ctx);
                if (!report_writer)
                {
                    token->complete_task(1);
                }
                read_ok_.current()++;
                read_nr_.current()++;
            }
        }
    }
    void run_cacheline(BLS &,
                       const Config &conf,
                       CoroContext &ctx,
                       ::bench::StopToken::pointer token,
                       size_t data_nr,
                       size_t data_size,
                       bool w)
    {
        std::ignore = conf;
        PrvCoroCtx prv;

        auto sync_type = conf.get<enum Sync>("sync");
        auto total_size = actual_data_size(data_size, sync_type);

        prv.lock_buf = dsm_->get_rdma_buffer(sizeof(uint64_t));
        prv.unlock_buf = dsm_->get_rdma_buffer(sizeof(uint64_t));
        prv.write_buf = dsm_->get_rdma_buffer(total_size);
        prv.read_buf = dsm_->get_rdma_buffer(total_size);
        bool report_writer = conf.get<bool>("report_writer");

        std::vector<char> read_obj_buffer(data_size);

        while (!token->stop_requested())
        {
            auto ith = g_.current()->Next();
            DCHECK_LT(ith, data_nr);
            auto ith_data_gaddr = data_ + total_size * ith;

            CVObj cv_obj(data_size);
            auto lock_gaddr = cv_obj.lock_gaddr(ith_data_gaddr);
            if (w)
            {
                if (try_lock(lock_gaddr, ctx, prv))
                {
                    char *buf = prv.write_buf.buffer;
                    char magic_ch = fast_pseudo_rand_printable_char();
                    // CVObj::v_t v = fast_pseudo_rand_int();
                    cv_obj.fill_local_buf(buf, magic_ch);
                    dsm_->write_sync(buf, ith_data_gaddr, total_size, &ctx);
                    write_ok_.current()++;

                    if (report_writer)
                    {
                        token->complete_task(1);
                    }
                }
                else
                {
                    // lock fail, do nothing
                }
                write_nr_.current()++;
            }
            else
            {
                char *buf = prv.read_buf.buffer;
                dsm_->read_sync(buf, ith_data_gaddr, total_size, &ctx);
                // check consistency here
                char expect_ch = buf[0];
                auto nolock_size = CVObj::cal_object_occupy_size(data_size);
                bool match = true;
                for (size_t i = 0; i < nolock_size; ++i)
                {
                    if (buf[i] != expect_ch)
                    {
                        match = false;
                        break;
                    }
                }
                read_nr_.current()++;
                if (match)
                {
                    // now we need to recover the obj
                    ssize_t remain = data_size;
                    char *dest = read_obj_buffer.data();
                    const char *src = prv.read_buf.buffer;
                    while (remain)
                    {
                        auto block =
                            std::min(remain, (ssize_t) CVObj::cl_data_size());
                        memcpy(dest, src, block);
                        dest += block;
                        src += 64;
                        remain -= block;
                    }

                    read_ok_.current()++;
                    if (!report_writer)
                    {
                        token->complete_task(1);
                    }
                }
            }
        }
    }

    void run_crc(BLS &,
                 const Config &conf,
                 CoroContext &ctx,
                 ::bench::StopToken::pointer token,
                 size_t data_nr,
                 size_t data_size,
                 bool w)
    {
        std::ignore = conf;
        PrvCoroCtx prv;

        auto sync_type = conf.get<enum Sync>("sync");
        auto total_size = actual_data_size(data_size, sync_type);

        prv.lock_buf = dsm_->get_rdma_buffer(sizeof(uint64_t));
        prv.unlock_buf = dsm_->get_rdma_buffer(sizeof(uint64_t));
        prv.write_buf = dsm_->get_rdma_buffer(total_size);
        prv.read_buf = dsm_->get_rdma_buffer(total_size);
        bool report_writer = conf.get<bool>("report_writer");

        while (!token->stop_requested())
        {
            auto ith = g_.current()->Next();
            DCHECK_LT(ith, data_nr);
            auto ith_data_gaddr = data_ + total_size * ith;
            CrcObj crc_obj(ith_data_gaddr, data_size);
            // auto crc_gaddr = crc_obj.CRC().first;
            auto lock_gaddr = crc_obj.lock().first;
            // auto object_gaddr = crc_obj.object().first;
            if (w)
            {
                if (try_lock(lock_gaddr, ctx, prv))
                {
                    char *buf = prv.write_buf.buffer;
                    char magic_ch = fast_pseudo_rand_printable_char();
                    memset(buf, magic_ch, data_size);
                    uint64_t crc = CityHash64(buf, data_size);
                    memcpy(buf + data_size, &crc, sizeof(crc));
                    uint64_t lock_val = 0;
                    memcpy(buf + data_size + sizeof(uint64_t),
                           &lock_val,
                           sizeof(lock_val));
                    dsm_->write_sync(buf, ith_data_gaddr, total_size, &ctx);
                    write_ok_.current()++;

                    if (report_writer)
                    {
                        token->complete_task(1);
                    }
                }
                else
                {
                    // lock fail, do nothing
                }
                write_nr_.current()++;
            }
            else
            {
                char *buf = prv.read_buf.buffer;
                dsm_->read_sync(buf, ith_data_gaddr, total_size, &ctx);
                uint64_t get_crc = 0;
                memcpy(&get_crc, buf + data_size, sizeof(get_crc));
                uint64_t actual_crc = CityHash64(buf, data_size);
                if (get_crc == actual_crc)
                {
                    // check_consistency(buf, data_size);
                    if (!report_writer)
                    {
                        token->complete_task(1);
                    }
                    read_ok_.current()++;
                }
                read_nr_.current()++;
            }
        }
    }
    bool try_lock(GlobalAddress lock_gaddr,
                  CoroContext &ctx,
                  const PrvCoroCtx &prv)
    {
        bool ret = dsm_->cas_sync(
            lock_gaddr, 0, 1, (uint64_t *) prv.lock_buf.buffer, &ctx);
        return ret;
    }
    void unlock(GlobalAddress lock_gaddr,
                CoroContext &ctx,
                const PrvCoroCtx &prv)
    {
        dsm_->write(
            prv.unlock_buf.buffer, lock_gaddr, sizeof(uint64_t), false, &ctx);
    }
    bool try_dm_lock(size_t ith, CoroContext &ctx, const PrvCoroCtx &prv)
    {
        GlobalAddress lock_gaddr;
        lock_gaddr.nodeID = meta_.nodeID;
        lock_gaddr.offset = ith * sizeof(uint64_t);
        bool ret = dsm_->cas_dm_sync(
            lock_gaddr, 0, 1, (uint64_t *) prv.lock_buf.buffer, &ctx);
        return ret;
    }
    void dm_unlock(size_t ith, CoroContext &ctx, const PrvCoroCtx &prv)
    {
        GlobalAddress lock_gaddr;
        lock_gaddr.nodeID = meta_.nodeID;
        lock_gaddr.offset = ith * sizeof(uint64_t);
        memset(prv.unlock_buf.buffer, 0, sizeof(uint64_t));
        dsm_->write_dm(
            prv.unlock_buf.buffer, lock_gaddr, sizeof(uint64_t), false, &ctx);
    }
    void check_consistency(char *buf, size_t size)
    {
        char expect = buf[0];
        for (size_t i = 0; i < size; ++i)
        {
            CHECK_EQ(buf[i], expect) << "** not consistency " << std::endl
                                     << util::Hexdump(buf, size);
        }
    }

    void benchmark_coroutine(BLS &bls,
                             const Config &conf,
                             CoroContext &ctx,
                             ::bench::StopToken::pointer token,
                             bool) override
    {
        auto dsm = get_dsm();
        size_t write_ratio = conf.get<size_t>("write_ratio");
        size_t thread_nr = conf.thread_nr();
        size_t writer_nr = thread_nr * write_ratio / 100.0;
        size_t data_nr = conf.get<size_t>("data_nr");
        size_t data_size = conf.get<size_t>("data_size");
        auto tid = util::get_thread_id();
        bool is_writer = tid < writer_nr;
        auto sync = conf.get<enum Sync>("sync");
        if (sync == kCRC)
        {
            run_crc(bls, conf, ctx, token, data_nr, data_size, is_writer);
        }
        else if (sync == kCOW)
        {
            run_cow(bls, conf, ctx, token, data_nr, data_size, is_writer);
        }
        else if (sync == kCacheline)
        {
            run_cacheline(bls, conf, ctx, token, data_nr, data_size, is_writer);
        }
        else
        {
            LOG(FATAL) << "TODO";
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

    Perthread<std::unique_ptr<util::Generator>> g_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    Experiment exp;

    bench::ConfigFactory f;
    // f.configure_thread_nr({1, 4, 8, 16});
    f.configure_thread_nr({kMaxAppThread});
    // f.configure_thread_nr({2});
    f.configure_coro_nr({3});
    f.add_option<size_t>("data_size", {64, kInternalPageSize});
    // f.add_option<size_t>("data_size", {2_MB});
    // f.add_option<size_t>("data_size", {64, kInternalPageSize});
    f.add_option<size_t>("data_nr", {FLAGS_data_nr});
    f.add_option<size_t>("write_ratio", {5});
    // f.add_option<enum Sync>("sync", {kCOW, kCRC, kCacheline});
    f.add_option<enum Sync>("sync", {kCRC, kCacheline});
    // f.add_option<enum Sync>("sync", {kCacheline});
    f.add_option<bool>("report_writer", {false});
    auto configs = f.generate_configs();

    exp.configure_monitor(400ms, 8);
    exp.launch_coroutines(configs);

    LOG(INFO) << "PASS.";
}