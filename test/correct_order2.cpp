#include <immintrin.h>
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

DEFINE_uint32(bufsize, 64 * 2, "the size of buffer");

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

enum Who
{
    CPU,
    RNIC
};

std::ostream &operator<<(std::ostream &os, Who who)
{
    switch (who)
    {
    case Who::CPU:
        os << "CPU";
        break;
    case Who::RNIC:
        os << "RNIC";
        break;
    }
    return os;
}

template <typename T>
struct SegT
{
    T val;
    size_t size;
};
template <typename T>
std::ostream &operator<<(std::ostream &os, const SegT<T> &seg)
{
    os << "(" << util::InlinedHexdump((void *) &seg.val, sizeof(T)) << ", "
       << seg.size << ")";
    return os;
}

template <typename T>
class OrderingInfo
{
public:
    using Seg = SegT<T>;
    const auto &segments() const
    {
        return segs_;
    }
    bool anomaly() const
    {
        return segs_.size() > 1;
    }
    OrderingInfo(const char *buf, size_t size) : buf_(buf), size_(size)
    {
        CHECK_GE(size, sizeof(T));
        const T *buf_ = (const T *) buf;
        size_t last_idx = 0;
        T last_val = buf_[0];
        for (size_t i = 1; i < size / sizeof(T); ++i)
        {
            T cur_last_val = buf_[i - 1];
            T cur_val = buf_[i];
            if (cur_last_val != cur_val)
            {
                segs_.emplace_back(Seg{.val = cur_last_val,
                                       .size = (i - last_idx) * sizeof(T)});
                last_idx = i;
                last_val = cur_val;
            }
        }
        segs_.emplace_back(
            Seg{.val = last_val, .size = size - last_idx * sizeof(T)});
    }
    uint64_t min_max_diff() const
    {
        CHECK(!segs_.empty());
        T min_val = segs_.front().val;
        T max_val = min_val;
        for (const auto &seg : segs_)
        {
            min_val = std::min(min_val, seg.val);
            max_val = std::max(max_val, seg.val);
        }
        return max_val - min_val;
    }
    int reverse_nr() const
    {
        CHECK(!segs_.empty());
        int reverse_nr = 0;
        T cur_val = segs_.front().val;
        for (const auto &seg : segs_)
        {
            T ptr_val = seg.val;
            if (ptr_val > cur_val)
            {
                reverse_nr++;
                cur_val = ptr_val;
            }
        }
        return reverse_nr;
    }
    std::pair<size_t, size_t> seg_sizes() const
    {
        CHECK(!segs_.empty());
        size_t min_segsize = segs_.front().size;
        size_t max_segsize = min_segsize;
        for (const auto &seg : segs_)
        {
            min_segsize = std::min(min_segsize, seg.size);
            max_segsize = std::max(max_segsize, seg.size);
        }
        return {min_segsize, max_segsize};
    }
    const char *buffer() const
    {
        return buf_;
    }
    size_t size() const
    {
        return size_;
    }
    bool cl_consitent() const
    {
        auto [min_size, _] = seg_sizes();
        if (min_size < 64)
        {
            return false;
        }
        return true;
    }

private:
    std::vector<Seg> segs_;
    const char *buf_;
    size_t size_;
};

template <typename T>
std::ostream &operator<<(std::ostream &os, const OrderingInfo<T> &o)
{
    bool cl_consistent = true;
    auto [min_segsize, max_segsize] = o.seg_sizes();
    if (min_segsize < 64)
    {
        cl_consistent = false;
    }
    int reverse_nr = o.reverse_nr();
    auto diff = o.min_max_diff();
    os << "{OrderInfo cl_const: " << util::pre(cl_consistent)
       << ", rev: " << reverse_nr << ", diff: " << diff << "}";
    // os << " debug: " << util::InlinedHexdump(o.buffer(), o.size());
    os << " " << util::pre(o.segments());

    return os;
}

template <typename T>
class BufferModifier
{
public:
    BufferModifier(void *buf, size_t size) : buf_(buf), size_(size)
    {
        // tmp_buf_ = (char *) malloc(size);
        tmp_buf_ = (char *) aligned_alloc(64, size);
        memcpy(tmp_buf_, buf, size);
    }
    ~BufferModifier()
    {
        if (tmp_buf_)
        {
            free(tmp_buf_);
        }
    }
    void local_advance()
    {
        T *t_buf_ = (T *) buf_;
        for (size_t i = 0; i < size_ / sizeof(T); i++)
        {
            t_buf_[i]++;
        }
    }
    void sync_advance()
    {
        // NOTE: don't do uint64_t++, it is wrong if you check at
        // byte-granularity
        // NOTE: memcpy does not ensure 64B-atomicity.
        // NOTE: std::atomic<uint64_t> does not ensure 64B-atomicity
        T *t_tmp_buf_ = (T *) tmp_buf_;
        for (size_t i = 0; i < size_ / sizeof(T); ++i)
        {
            t_tmp_buf_[i]++;
        }
        // memcpy(buf_, tmp_buf_, size_);
        // cl_atomic_memcpy(buf_, tmp_buf_, size_);
        // avx_memcpy(buf_, tmp_buf_, size_);
        util::memcpy_atomic_64b(buf_, tmp_buf_, size_);
    }
    // NOTE: wrong
    void avx_memcpy(void *dest, const void *src, size_t len)
    {
        CHECK_EQ((uint64_t) dest % 64, 0)
            << (void *) dest << " dest not aligned";
        CHECK_EQ((uint64_t) src % 64, 0) << (void *) src << " src not aligned";
        CHECK_EQ(len % sizeof(__m512i), 0);
        for (size_t i = 0; i < len; i += sizeof(__m512i))
        {
            char *dest_addr = (char *) dest + i;
            char *src_addr = (char *) src + i;
            __m512i src_val;
            memcpy(&src_val, src_addr, sizeof(__m512i));
            // _mm512_store_ps(dest_addr, src_val);
            _mm512_store_si512(dest_addr, src_val);
        }
    }
    // NOTE: wrong
    void cl_atomic_memcpy(void *dest, const void *src, size_t len)
    {
        // OrderingInfo<unsigned char> check((const char *) src, len);
        // CHECK(check.cl_consitent());

        CHECK_EQ(len % sizeof(uint64_t), 0);
        std::atomic<uint64_t> *dest_ = (std::atomic<uint64_t> *) dest;
        // volatile uint64_t *dest_ = (volatile uint64_t *) dest;
        uint64_t *src_ = (uint64_t *) src;
        for (size_t i = 0; i < len / sizeof(uint64_t); ++i)
        {
            dest_[i].store(src_[i]);
            // dest_[i] = src_[i];
        }
    }

    T *buffer()
    {
        return (T *) buf_;
    }
    const T *buffer() const
    {
        return (const T *) buf_;
    }
    size_t size() const
    {
        return size_;
    }

private:
    void *buf_;
    size_t size_;
    char *tmp_buf_{};
};

struct Spec
{
    // using TLS = ThreadContext;
};

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
    }
    void exit() override
    {
        Base::exit();
    }
    void thread_init(BLS &bls, size_t thread_nr) override
    {
        dsm_->registerThread();
        Base::thread_init(bls, thread_nr);
    }

    void modify(BLS &bls,
                const Config &conf,
                Who role,
                ::bench::StopToken::pointer token)
    {
        std::ignore = bls;

        auto buf_size = conf.get<size_t>("buf_size");
        GlobalAddress gaddr;
        gaddr.nodeID = dsm_->get_node_id();
        gaddr.offset = 0;
        auto rdma_buf = dsm_->get_rdma_buffer(buf_size);
        memset(rdma_buf.buffer, 0, buf_size);

        char *base_addr = (char *) dsm_->get_base_addr();
        CHECK_EQ((uint64_t) base_addr % 64, 0);
        memset(base_addr, 0, buf_size);

        while (!token->stop_requested())
        {
            if (role == RNIC)
            {
                BufferModifier<unsigned char> m(rdma_buf.buffer, buf_size);
                m.local_advance();

                dsm_->prepare_write(
                    (char *) m.buffer(), gaddr, m.size(), false, nullptr);
                dsm_->commit(nullptr);
            }
            else
            {
                BufferModifier<unsigned char> m(base_addr, buf_size);
                while (!token->stop_requested())
                {
                    m.sync_advance();
                }
            }
            token->complete_task(1);
        }
    }
    void observe(BLS &bls,
                 const Config &conf,
                 Who role,
                 ::bench::StopToken::pointer token)
    {
        std::ignore = bls;

        auto buf_size = conf.get<size_t>("buf_size");
        GlobalAddress gaddr;
        gaddr.nodeID = dsm_->get_node_id();
        gaddr.offset = 0;
        auto rdma_buf = dsm_->get_rdma_buffer(buf_size);
        memset(rdma_buf.buffer, 0, buf_size);

        char *copy_buf = (char *) aligned_alloc(64, buf_size);
        while (!token->stop_requested())
        {
            if (role == RNIC)
            {
                dsm_->prepare_read(
                    rdma_buf.buffer, gaddr, buf_size, false, nullptr);
                dsm_->commit(nullptr);
                OrderingInfo<unsigned char> rnic_info(rdma_buf.buffer,
                                                      buf_size);
                // LOG_EVERY_N(INFO, (int) 10_K) << "RNIC: " << PRE(info);
                // LOG_IF(INFO, rnic_info.anomaly()) << PRE(rnic_info);
                // if (rnic_info.reverse_nr())
                // {
                //     LOG(INFO) << "reverse: " << PRE(rnic_info);
                // }
                // auto diff = rnic_info.min_max_diff();
                // auto no_report = std::numeric_limits<unsigned char>::max();
                // if (diff > 1 && diff != no_report)
                // {
                //     LOG(INFO) << PRE(rnic_info);
                // }
                // if (!rnic_info.cl_consitent())
                // {
                //     LOG(INFO) << PRE(rnic_info);
                // }
            }
            else
            {
                char *observe_buffer = (char *) dsm_->get_base_addr();
                util::memcpy_atomic_64b(copy_buf, observe_buffer, buf_size);

                OrderingInfo<unsigned char> cpu_info(copy_buf, buf_size);
                // LOG_IF(INFO, cpu_info.anomaly()) << PRE(cpu_info);
                // LOG_EVERY_N(INFO, (int) 10_M) << "CPU: " << PRE(info);
                // auto diff = cpu_info.min_max_diff();
                // auto no_report = std::numeric_limits<unsigned char>::max();
                // if (diff > 1 && diff != no_report)
                // {
                //     LOG(INFO) << PRE(cpu_info);
                // }
                if (!cpu_info.cl_consitent())
                {
                    LOG(INFO) << PRE(cpu_info);
                }
                // if (cpu_info.reverse_nr())
                // {
                //     LOG(INFO) << PRE(cpu_info);
                // }
            }
            token->complete_task(1);
        }

        dsm_->put_rdma_buffer(std::move(rdma_buf));
        free(copy_buf);
    }
    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        // auto observer = conf.get<Who>("observer");
        auto modifier = conf.get<Who>("modifier");

        [[maybe_unused]] auto thread_nr = conf.thread_nr();

        bool is_modify = bls.id().is_master_thread();

        if (is_modify)
        {
            modify(bls, conf, modifier, token);
        }
        else
        {
            bool is_cpu = bls.id().select_thread(2);
            if (is_cpu)
            {
                observe(bls, conf, CPU, token);
            }
            else
            {
                observe(bls, conf, RNIC, token);
            }
        }
    }

private:
    DSM::pointer dsm_;
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
    // f.add_option<Order>("order", {WW});
    // f.add_option<Who>("observer", {RNIC});
    f.add_option<Who>("modifier", {RNIC});
    // f.add_option<size_t>("buf_size", {FLAGS_bufsize});
    f.add_option<size_t>("buf_size", {4_KB});
    // f.add_option<Order>("order", {RR});
    auto configs = f.generate_configs();

    exp.configure_monitor(1s, 15);
    exp.launch(configs);

    LOG(INFO) << "PASS.";
}

// If RNIC writes
// RNIC observers: 64B-atomic, but 64B may be reordered
// but never witness > 1 max_min difference
// CPU observers: not 64B-atomic, 64B-reorder

// If CPU writes
// RNIC observers: never ensures 64B atomicity (no any atomicity), and each 64B
// may be reordered