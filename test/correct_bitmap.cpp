#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <unordered_set>

#include "DSM.h"
#include "GlobalAddress.h"
#include "avis/bitmap.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Util.h"
#include "util/bits.h"
#include "util/gflags_def.h"

class NullContainer
{
public:
    template <typename T>
    void insert(T &)
    {
    }
    template <typename T>
    void emplace_back(T &)
    {
    }
    template <typename T>
    void push_back(T &)
    {
    }
};

class Test
{
public:
    Test(DSM::pointer dsm, size_t size, size_t obj_size)
        : dsm_(dsm), size_(size), obj_size_(obj_size)
    {
        data_ = dsm_->alloc(size_);
        auto ptl =
            std::make_shared<avis::PTL>(dsm, dsm->get_node_id(), nullptr);
        slab_ = std::make_unique<avis::BitmapSlab>(
            dsm_, data_, size, obj_size, ptl, nullptr);
        rdma_buf_ = dsm_->get_rdma_buffer(slab_->bitmap_size() +
                                          sizeof(avis::BitmapHeader));
    }
    ~Test()
    {
        dsm_->free(data_, size_);
    }

    void run()
    {
        fragement(1, 1, 1.0);
        reset();

        fragement(1, 1, 0.5);
        reset();

        fragement(std::min((size_t) 1, slab_->block_nr() / 2), 1, 0.5);
        reset();

        fragement(std::min((size_t) 1, slab_->block_nr() / 2), 20, 0.8);
        reset();

        fragement(slab_->block_nr(), 30, 0.7);
        reset();

        fragement(std::min((size_t) 1, slab_->block_nr() / 2), 100, 0.5);
        reset();
    }

    void hello()
    {
        NullContainer c;
        alloc(c);
        dealloc(allocated_);
        LOG(INFO) << "PASS hello";
    }

    void fragement(size_t block_nr, size_t loop_nr, double free_percent)
    {
        std::vector<GlobalAddress> addrs;
        addrs.reserve(slab_->object_nr());
        for (size_t i = 0; i < loop_nr; ++i)
        {
            // Do a `addrs = allocated`
            addrs.clear();
            for (auto &addr : allocated_)
            {
                addrs.push_back(addr);
            }

            alloc(addrs, block_nr);
            auto to_free = addrs;
            std::random_shuffle(to_free.begin(), to_free.end());
            auto expected_size = to_free.size() * (1.0 - free_percent);
            while (to_free.size() >= expected_size && !to_free.empty())
            {
                to_free.pop_back();
            }
            dealloc(to_free);
        }
        LOG(INFO) << fmt::format(
            "PASS fragment({}, {}, {})", block_nr, loop_nr, free_percent);
    }

    void reset()
    {
        memset(rdma_buf_.buffer, 0, rdma_buf_.size);
        memcpy(rdma_buf_.buffer,
               &avis::BitmapHeader::kMagic,
               sizeof(avis::BitmapHeader::kMagic));
        dsm_->prepare_write(rdma_buf_.buffer,
                            data_,
                            slab_->bitmap_size() + sizeof(avis::BitmapHeader),
                            false,
                            nullptr);
        dsm_->commit();
        allocated_.clear();
        slab_->reset();
    }

    template <typename Container>
    void alloc(Container &c, size_t block_nr = 1, bool do_check = true)
    {
        slab_->alloc(
            [&c, this](GlobalAddress addr)
            {
                c.emplace_back(addr);
                CHECK(allocated_.insert(addr).second)
                    << PRE(addr) << " already exist in set";
            },
            block_nr,
            false);

        if (do_check)
        {
            check();
        }
    }
    template <typename Container>
    void dealloc(const Container &addrs, bool do_check = true)
    {
        // avis::ContainerGen gen(addrs);
        auto it = addrs.begin();
        auto generator = [&addrs, &it]() -> std::optional<GlobalAddress>
        {
            if (unlikely(it == addrs.end()))
            {
                return std::nullopt;
            }
            auto ret = *it;
            it++;
            return ret;
        };
        slab_->free(generator);
        for (auto addr : addrs)
        {
            CHECK(allocated_.erase(addr));
        }

        if (do_check)
        {
            check();
        }
    }

    void check()
    {
        auto alloc = slab_->get_allocated();
        CHECK_EQ(alloc.size(), allocated_.size());
    }

private:
    DSM::pointer dsm_;
    GlobalAddress data_;
    size_t size_;
    [[maybe_unused]] size_t obj_size_;
    std::unique_ptr<avis::BitmapSlab> slab_;

    Buffer rdma_buf_;

    std::set<GlobalAddress> allocated_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig config;
    config.worker_nr = 0;

    auto dsm = DSM::getInstance(config);
    dsm->registerThread();

    Test test(dsm, 4_KB, 8);
    test.run();

    auto space = dsm->alloc(4_KB);
    auto ptl = std::make_shared<avis::PTL>(dsm, dsm->get_node_id(), nullptr);
    avis::BitmapSlab slab(dsm, space, 4_KB, 8, ptl, nullptr);
    slab.init();
    std::vector<GlobalAddress> all;
    slab.alloc([&](GlobalAddress addr) { all.push_back(addr); },
               slab.block_nr(),
               true);
    CHECK_EQ(all.size(), slab.object_nr());

    for (const auto &addr : all)
    {
        CHECK_GE(addr.offset, space.offset);
        CHECK_LE(addr.offset + slab.object_size(), space.offset + 4_KB);
    }

    LOG(INFO) << "PASS";
    return 0;
}