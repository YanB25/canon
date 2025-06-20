#pragma once
#include <initializer_list>
#include <iterator>
#include <limits>

#include "./config.h"
#include "./ptl.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "Metrics.h"
#include "util/Likely.h"
#include "util/PreUtil.h"
#include "util/Rand.h"
#include "util/Util.h"
#include "util/bits.h"

namespace avis
{
struct BitmapHeader
{
    constexpr static uint32_t kMagic = 0xabcdabcd;
    uint32_t obj_size;
    uint32_t magic{};
    bool valid() const
    {
        return magic == kMagic;
    }
};

class BitmapSlab
{
    // Layout:
    // [Header] [Bitmap] [Obj]...[Obj]
public:
    constexpr static bool kReport = false;
    BitmapSlab(DSM::pointer dsm,
               GlobalAddress addr,
               size_t total_size,
               size_t obj_size,
               std::shared_ptr<PTL> ptl,
               CoroContext *ctx)
        : dsm_(dsm),
          addr_(addr),
          total_size_(total_size),
          obj_size_(obj_size),
          ptl_(ptl),
          ctx_(ctx)
    {
        LOG_IF(INFO, kReport)
            << "[Bitmap] opened at " << PRE(addr, total_size, obj_size);

        CHECK(!addr.is_null());

        DCHECK_GE(obj_size, 8);
        DCHECK_EQ(addr.offset % 8, 0)
            << "** addr not 8-aligned: unable to perform CAS";
        CHECK_GT(total_size, 0);

        // solving equation: how much object we can store?
        // header + x * sizeof(obj) + (x+7)/8 == total_size
        // This equation does not consider slabs to be aligned,
        // so the obj_nr_ is a maximum value.
        obj_nr_ =
            (8 * total_size_ - 8 * header_size() - 7) / (8 * obj_size_ + 1);
        // LOG(INFO) << PRE(total_size, obj_size, obj_nr_);
        // Adjust to alignment: slabs should be 8B aligned
        auto slab_start_offset = header_size() + (obj_nr_ + 7) / 8;
        slab_start_offset = util::round_up_aligned(slab_start_offset, 8);
        // LOG(INFO) << PRE(slab_start_offset);
        // remain: the space for slabs
        auto remain = total_size - slab_start_offset;
        obj_nr_ = std::min(obj_nr_, remain / obj_size_);

        obj_addr_ = addr_ + slab_start_offset;

        // After this complex calculation, check that we did not make any
        // error
        DCHECK_LE(obj_addr_ + obj_size_ * obj_nr_, addr_ + total_size_)
            << "** Overflow detected. This invariation should never be "
               "broken.";

        // init rdma related
        rdma_buffer_ = dsm_->get_rdma_buffer(bitmap_size() + header_size());
        memset(rdma_buffer_.buffer, 0, rdma_buffer_.size);
        free_rdma_buffer_ = dsm_->get_rdma_buffer(bitmap_size());
        memset(free_rdma_buffer_.buffer, 0, bitmap_size());

        do_read_meta(true);
    }
    /**
     * init initializes the remote spaces.
     * This function should be called only once by one thread.
     */
    void init()
    {
        auto bitmap_sz = bitmap_size();
        auto bitmap_buf =
            dsm_->get_rdma_buffer(bitmap_sz + sizeof(BitmapHeader));
        memset(bitmap_buf.buffer, 0, bitmap_sz + sizeof(BitmapHeader));
        dsm_->prepare_write(
            bitmap_buf.buffer, bitmap_addr(), bitmap_sz, false, ctx_);
        dsm_->commit(ctx_);
        rm_.write(bitmap_sz);
        dsm_->put_rdma_buffer(std::move(bitmap_buf));

        // fill-in the magic into the header
        // which is viewed as a commit point of readiness
        BitmapHeader &header = *(BitmapHeader *) rdma_buffer_.buffer;
        header.obj_size = object_size();
        header.magic = BitmapHeader::kMagic;
        dsm_->prepare_write(
            rdma_buffer_.buffer, addr_, sizeof(BitmapHeader), false, ctx_);
        dsm_->commit(ctx_);
        rm_.write(sizeof(BitmapHeader));
    }
    void report() const
    {
        LOG(INFO) << "[Slab] mm: " << am_ << ", IO: " << rm_;
    }
    std::pair<AllocMetrics, RemoteMetrics> metrics() const
    {
        return std::make_pair(am_, rm_);
    }
    void metric_reset()
    {
        rm_.reset();
        am_.reset();
    }

    bool contains(GlobalAddress raddr) const
    {
        auto obj_start_addr = object_addr();
        if (obj_start_addr.offset <= raddr.offset &&
            raddr.offset <= obj_start_addr.offset + obj_nr_ * obj_size_)
        {
            return true;
        }
        return false;
    }
    GlobalAddress meta_raddr() const
    {
        return addr_;
    }

    void reset()
    {
        memset(rdma_buffer_.buffer, 0, rdma_buffer_.size);
        memset(free_rdma_buffer_.buffer, 0, free_rdma_buffer_.size);
    }
    std::vector<GlobalAddress> get_allocated() const
    {
        std::vector<GlobalAddress> ret;
        auto bs = bitmap_bs();
        for (size_t obj_id = 0; obj_id < obj_nr_; ++obj_id)
        {
            if (bs.is_set(obj_id))
            {
                ret.push_back(obj_id_to_raddr(obj_id));
            }
        }

        return ret;
    }
    struct Dump
    {
        size_t object_size_{};
        size_t object_nr_{};
        size_t allocated_nr_{};
        bool all_allocated() const
        {
            return allocated_nr_ == object_nr_;
        }
        bool not_allocated() const
        {
            return allocated_nr_ == 0;
        }
        bool partial_allocated() const
        {
            return !all_allocated() && !not_allocated();
        }
        size_t total_size() const
        {
            return object_size_ * object_nr_;
        }
        size_t allocated_size() const
        {
            return allocated_nr_ * object_size_;
        }
        size_t available_size() const
        {
            return total_size() - allocated_size();
        }
        size_t allocated_nr() const
        {
            return allocated_nr_;
        }
        size_t total_nr() const
        {
            return object_nr_;
        }
        size_t available_nr() const
        {
            return total_nr() - allocated_nr();
        }
    };
    /**
     * This is loosely synchronized.
     */
    Dump dump() const
    {
        Dump ret;
        ret.object_nr_ = object_nr();
        ret.object_size_ = header().obj_size;
        ret.allocated_nr_ = 0;
        const auto &bs = bitmap_bs();
        for (size_t i = 0; i < object_nr(); ++i)
        {
            if (bs.is_set(i))
            {
                ret.allocated_nr_++;
            }
        }
        return ret;
    }

    bool empty() const
    {
        auto bs = bitmap_bs();
        return bs.all_ones();
    }

    /**
     * alloc() allocates a batch of objects (for performance optimization).
     * @param insert_fn: the wait to return allocation: insert_fn(addr)
     * @param block_nr: How many 8B blocks to try to allocate
     * @param read_meta: whether or not update meta before allocation
     */
    template <typename Fn>
    size_t alloc(Fn &&insert_fn, size_t block_nr, bool read_meta = false)
    {
        // if dont read_meta, would witness *false* failure.
        if (!read_meta)
        {
            auto ret = do_alloc(insert_fn, block_nr, read_meta);
            if (unlikely(ret == 0))
            {
                // retry to avoid false failure
                return do_alloc(std::forward<Fn>(insert_fn),
                                block_nr,
                                true /* read_meta */);
            }
            return ret;
        }
        else
        {
            return do_alloc(std::forward<Fn>(insert_fn), block_nr, read_meta);
        }
    }
    size_t alloc(std::stack<GlobalAddress> &ret,
                 size_t block_nr,
                 bool read_meta = false)
    {
        return alloc([&ret](GlobalAddress addr) { ret.push(addr); },
                     block_nr,
                     read_meta);
    }
    size_t alloc(std::vector<GlobalAddress> &ret,
                 size_t block_nr,
                 bool read_meta = false)
    {
        return alloc([&ret](GlobalAddress addr) { ret.emplace_back(addr); },
                     block_nr,
                     read_meta);
    }
    size_t alloc(std::unordered_set<GlobalAddress> &ret,
                 size_t block_nr,
                 bool read_meta = false)
    {
        return alloc(
            [&ret](GlobalAddress addr) {
                bool ok = ret.emplace(addr).second;
                DCHECK(ok) << "** duplicated allocation detected";
            },
            block_nr,
            read_meta);
    }

    size_t block_nr() const
    {
        return bitmap_block_nr();
    }

    template <typename Fn>
    size_t do_alloc(Fn &&insert_fn, size_t block_nr, bool read_meta = false)
    {
        auto bs = bitmap_bs();
        if (unlikely(bs.all_ones()))
        {
            LOG_IF(INFO, kReport)
                << "[bitmap] Plan to update meta: possibly no remain objects.";
            read_meta = true;
        }
        if (read_meta)
        {
            do_read_meta(true /* commit */);
        }
        if (unlikely(bs.all_ones()))
        {
            return 0;
        }

        block_nr = std::min(block_nr, bitmap_block_nr());
        std::vector<int> blocks;
        blocks.reserve(block_nr);
        // select randomly
        auto select_idx = fast_pseudo_rand_int(0, bitmap_block_nr() - 1);
        for (size_t i = 0; i < bitmap_block_nr(); ++i)
        {
            auto target_block = (select_idx + i) % bitmap_block_nr();
            uint64_t bitmap = bitmap_block(target_block);
            if (likely(util::has_zeros(bitmap)))
            {
                blocks.push_back(target_block);
            }
            // okay, we got enough blocks
            if (blocks.size() >= block_nr)
            {
                break;
            }
        }

        if (unlikely(blocks.empty()))
        {
            return 0;
        }

        auto nr = alloc_batch(blocks, std::forward<Fn>(insert_fn));
        return nr;
    }

    template <typename Fn>
    size_t alloc_batch(const std::vector<int> &blocks, Fn &&insert_fn)
    {
        LOG_IF(INFO, kReport)
            << "[bitmap] allocation execution plan: from " << PRE(blocks);
        std::vector<Buffer> buffers;
        buffers.reserve(blocks.size());

        auto bitmap_raddr = bitmap_addr();
        size_t ongoing = 0;

        size_t allocated_nr = 0;
        for (int block_id : blocks)
        {
            auto bitmap_gaddr = bitmap_raddr + sizeof(uint64_t) * block_id;
            uint64_t *pcompare = pbitmap_block(block_id);
            uint64_t compare = *pcompare;
            uint64_t mask = 0xffffffffffffffff;
            uint64_t swap = mask;
            auto rdma_buffer = dsm_->get_rdma_buffer(sizeof(uint64_t));
            buffers.emplace_back(std::move(rdma_buffer));
            write_ptl(bitmap_raddr, block_id);
            LOG_IF(INFO, kReport) << "[bitmap] Issuing CAS at " << PRE(block_id)
                                  << " with compare: " << (void *) compare;
            dsm_->prepare_cas(bitmap_gaddr,
                              sizeof(uint64_t),
                              compare,
                              mask,
                              swap,
                              mask,
                              buffers.back().buffer,
                              false,
                              ctx_);
            rm_.cas(sizeof(uint64_t), true);
            ongoing++;
            if (ongoing >= 8)
            {
                dsm_->commit(ctx_);
                ongoing = 0;
            }
        }
        if (ongoing)
        {
            dsm_->commit(ctx_);
            ongoing = 0;
        }

        for (size_t i = 0; i < blocks.size(); ++i)
        {
            auto block_id = blocks[i];
            const auto &buffer = buffers[i];

            uint64_t old_val = *(uint64_t *) buffer.buffer;
            uint64_t *pcompare = pbitmap_block(block_id);
            uint64_t compare = *pcompare;
            if (old_val == compare)
            {
                // CAS OKAY
                LOG_IF(INFO, kReport)
                    << "[bitmap] CAS OKAY at " << PRE(block_id)
                    << " nr: " << util::count_zeros(old_val);
                *pcompare = 0xffffffffffffffff;
                for (size_t j = 0; j < sizeof(uint64_t) * 8; ++j)
                {
                    auto test = 1ull << j;
                    if ((old_val & test) == 0)
                    {
                        auto object_id = block_id * sizeof(uint64_t) * 8 + j;
                        if (object_id < obj_nr_)
                        {
                            auto ret_raddr = obj_id_to_raddr(object_id);

                            // fill in the allocation here
                            std::forward<Fn>(insert_fn)(ret_raddr);

                            DCHECK(contains(ret_raddr))
                                << "** internal corruption: " << PRE(ret_raddr)
                                << " not belong to me.";

                            am_.record_alloc(obj_size_);
                            allocated_nr++;
                        }
                    }
                }
            }
            else
            {
                // CAS failed
                *pcompare = old_val;
                LOG_IF(INFO, kReport)
                    << "[bitmap] CAS FAILED at " << PRE(block_id)
                    << " got: " << (void *) old_val;
            }
        }
        return allocated_nr;
    }

    void meta_record_free(int obj_id)
    {
        util::BitsViewMut bs(rdma_buffer_.buffer + sizeof(BitmapHeader),
                             bitmap_size());
        bs.reset(obj_id);
    }
    size_t object_size() const
    {
        return obj_size_;
    }

    void free(std::unordered_set<GlobalAddress> &addr)
    {
        free([&addr]() -> std::optional<GlobalAddress> {
            if (unlikely(addr.empty()))
            {
                return std::nullopt;
            }
            auto it = addr.begin();
            auto ret = *it;
            addr.erase(it);
            return ret;
        });
    }

    template <Gen<GlobalAddress> Generator>
    void free(Generator &&gen)
    {
        memset(free_rdma_buffer_.buffer, 0, bitmap_size());
        util::BitsViewMut bs(free_rdma_buffer_.buffer, bitmap_size());

        std::set<size_t> to_free_object_id;

        while (auto raddr = gen())
        {
            if (!raddr)
            {
                break;
            }

            DCHECK(contains(*raddr))
                << "** addr " << *raddr << " not belongt to me";

            auto object_id = raddr_to_obj_id(*raddr);
            bs.set(object_id);
            meta_record_free(object_id);
            am_.record_dealloc(obj_size_);
            if constexpr (debug())
            {
                CHECK(to_free_object_id.insert(object_id).second)
                    << "** Why freeing twice?";
            }
        }

        size_t batch_limit = 8;
        size_t ongoing = 0;
        for (size_t block_id = 0; block_id < bitmap_block_nr(); ++block_id)
        {
            uint64_t *pbuffer =
                ((uint64_t *) free_rdma_buffer_.buffer) + block_id;
            // no need to free
            if (*pbuffer == 0)
            {
                continue;
            }
            auto gaddr = bitmap_addr() + sizeof(uint64_t) * block_id;

            uint64_t add_val = *pbuffer;
            uint64_t boundary = 0xffffffffffffffff;
            LOG_IF(INFO, kReport)
                << "[bitmap] Issuing FAA at block " << block_id << ": slots "
                << (void *) add_val << ", nr: " << util::count_ones(add_val);
            dsm_->prepare_faa(gaddr,
                              sizeof(uint64_t),
                              add_val,
                              boundary,
                              pbuffer,
                              false,
                              ctx_);
            rm_.faa(sizeof(uint64_t));
            ongoing++;
            if (ongoing >= batch_limit)
            {
                dsm_->commit(ctx_);
                ongoing = 0;
            }
        }
        if (ongoing)
        {
            dsm_->commit(ctx_);
            ongoing = 0;
        }
        if constexpr (debug())
        {
            // validate the free is correct
            for (auto obj_id : to_free_object_id)
            {
                CHECK_LT(obj_id, object_nr());
                CHECK(bs.is_set(obj_id))
                    << "** possible remote corruption: freeing obj " << obj_id
                    << " via FAA, but the remote bit is not set. Effective "
                       "addr: "
                    << obj_id_to_raddr(obj_id) << std::endl
                    << "bitmap's object_addr:" << object_addr();
            }
        }
    }

    size_t waste_bytes() const
    {
        return total_size_ - obj_nr_ * obj_size_;
    }

    ~BitmapSlab()
    {
        dsm_->put_rdma_buffer(std::move(rdma_buffer_));
        dsm_->put_rdma_buffer(std::move(free_rdma_buffer_));
    }

    void write_ptl(GlobalAddress bitmap_raddr, uint64_t block_id)
    {
        if (ptl_)
        {
            ptl_->record_bitmap_to_slab(bitmap_raddr, block_id);
        }
    }

    GlobalAddress header_addr() const
    {
        return addr_;
    }
    constexpr size_t header_size() const
    {
        return sizeof(BitmapHeader);
    }
    GlobalAddress bitmap_addr() const
    {
        return header_addr() + sizeof(BitmapHeader);
    }

    size_t bitmap_size() const
    {
        // each object occupies 1 b, but need to round up to byte
        auto sz = (object_nr() + 7) / 8;
        // round up to 8
        sz = util::round_up_aligned(sz, 8);
        return sz;
    }
    size_t bitmap_block_nr() const
    {
        return bitmap_size() / sizeof(uint64_t);
    }

    void do_read_meta(bool commit)
    {
        LOG_IF(INFO, kReport) << "[bitmap] Fetch meta.";
        dsm_->prepare_read(rdma_buffer_.buffer,
                           addr_,
                           bitmap_size() + sizeof(BitmapHeader),
                           false,
                           ctx_);
        if (commit)
        {
            dsm_->commit(ctx_);
        }
    }

    GlobalAddress object_addr() const
    {
        return obj_addr_;
    }
    size_t object_nr() const
    {
        return obj_nr_;
    }
    const BitmapHeader &header() const
    {
        return *(BitmapHeader *) rdma_buffer_.buffer;
    }

private:
    DSM::pointer dsm_;
    GlobalAddress addr_;
    size_t total_size_;
    size_t obj_size_;
    std::shared_ptr<PTL> ptl_;
    size_t obj_nr_;
    GlobalAddress obj_addr_;
    CoroContext *ctx_;

    // These are RDMA-related info
    Buffer rdma_buffer_;
    Buffer free_rdma_buffer_;

    // metrics
    RemoteMetrics rm_;
    AllocMetrics am_;

    util::BitsView bitmap_bs() const
    {
        return util::BitsView(rdma_buffer_.buffer + header_size(),
                              bitmap_size());
    }

    int raddr_to_obj_id(GlobalAddress raddr)
    {
        DCHECK(contains(raddr));
        auto obj_addr = object_addr();
        auto offset = raddr.offset - obj_addr.offset;

        DCHECK_EQ(offset % obj_size_, 0)
            << "base: " << obj_addr << ", raddr " << raddr << ", offset "
            << offset << ", obj_size: " << obj_size_;

        return offset / obj_size_;
    }
    GlobalAddress obj_id_to_raddr(int obj_id) const
    {
        return object_addr() + obj_id * obj_size_;
    }
    uint64_t *pbitmap_block(int block_id)
    {
        uint64_t *pblocks =
            (uint64_t *) (rdma_buffer_.buffer + sizeof(BitmapHeader));
        DCHECK_LT(block_id, bitmap_block_nr());
        return &pblocks[block_id];
    }
    uint64_t bitmap_block(int block_id)
    {
        return *pbitmap_block(block_id);
    }
};

inline std::ostream &operator<<(std::ostream &os, const BitmapSlab::Dump &dump)
{
    os << "{[" << dump.object_size_ << "]: Allocated " << dump.allocated_nr()
       << " / " << dump.object_nr_ << " ("
       << util::pre_byte(dump.allocated_size()) << " / "
       << util::pre_byte(dump.total_size()) << ")}";
    return os;
}
}  // namespace avis