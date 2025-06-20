#include "GlobalAddress.h"
#include "HugePageAlloc.h"
#include "avis/avis.h"
#include "bench/experiment.h"
#include "util/concept.h"
#include "util/gflags_def.h"
#include "util/stacktrace.h"

void test(DSM::pointer dsm)
{
    auto rdma_buf = dsm->get_rdma_buffer(32);
    auto gaddr = dsm->alloc(32);
    memset(rdma_buf.buffer, 0xff, 32);

    dsm->prepare_write(rdma_buf.buffer, gaddr, 32, false, nullptr);
    dsm->commit();

    char add_val[32];
    memset(add_val, 0xff, sizeof(add_val));
    char field_boundary[32];
    memset(field_boundary, 0xff, sizeof(field_boundary));
    dsm->prepare_faa(gaddr,
                     32,
                     (uint64_t) add_val,
                     (uint64_t) field_boundary,
                     rdma_buf.buffer,
                     false);
    dsm->commit();
    LOG(INFO) << "After FAA, rdma_buf: " << util::Hexdump(rdma_buf.buffer, 32);
    auto [nid, buf] = dsm->explain_gaddr(gaddr);
    CHECK_EQ(nid, dsm->get_node_id());
    LOG(INFO) << "After FAA, local_buf: " << util::Hexdump(buf, 32);
}

void cas_get(DSM::pointer dsm, GlobalAddress gaddr, size_t size)
{
    auto rdma_buf = dsm->get_rdma_buffer(size);
    std::vector<char> compare(size, 0);
    std::vector<char> swap(size, 0xff);
    std::vector<char> mask(size, 0xff);

    dsm->prepare_cas(gaddr,
                     size,
                     (uint64_t) compare.data(),
                     (uint64_t) mask.data(),
                     (uint64_t) swap.data(),
                     (uint64_t) mask.data(),
                     rdma_buf.buffer,
                     false,
                     nullptr);
    dsm->commit();

    LOG(INFO) << "[CAS] get_old: " << std::endl
              << util::Bindump(rdma_buf.buffer, size);
    uint64_t *tmp = (uint64_t *) malloc(size);
    memcpy(tmp, rdma_buf.buffer, size);
    for (size_t i = 0; i < size / sizeof(uint64_t); ++i)
    {
        tmp[i] = bswap64(tmp[i]);
    }
    LOG(INFO) << "[CAS] after htonll got: " << std::endl
              << util::Bindump(tmp, size);
    free(tmp);
    dsm->put_rdma_buffer(std::move(rdma_buf));
}

void faa_get(DSM::pointer dsm, GlobalAddress gaddr, size_t size)
{
    auto rdma_buf = dsm->get_rdma_buffer(size);
    std::vector<char> add_val(size, 0);
    std::vector<char> boundary(size, 0xff);

    dsm->prepare_faa(gaddr,
                     size,
                     (uint64_t) add_val.data(),
                     (uint64_t) boundary.data(),
                     rdma_buf.buffer,
                     false,
                     nullptr);
    dsm->commit();

    auto bs = util::BitsViewMut(rdma_buf.buffer, size);
    LOG(INFO) << "[FAA] get_old: " << std::endl << bs.bin_dump();
    bs.bswap64();
    LOG(INFO) << "[FAA] get_old after bswap64: " << std::endl << bs.bin_dump();

    dsm->put_rdma_buffer(std::move(rdma_buf));
}

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig config;
    config.relaxed_ordering = false;
    auto dsm = DSM::getInstance(config);
    dsm->registerThread();

    auto meta = dsm->alloc(2_MB);
    auto rdma_buf = dsm->get_rdma_buffer(2_MB);
    memset(rdma_buf.buffer, 0, 2_MB);
    dsm->prepare_write(rdma_buf.buffer, meta, 2_MB, false, nullptr);
    dsm->commit();

    std::vector<std::shared_ptr<avis::Publisher<int>>> ps;
    for (size_t i = 0; i < 8; ++i)
    {
        ps.emplace_back(
            std::make_shared<avis::Publisher<int>>(dsm, meta, 1_KB, nullptr));
    }

    for (size_t i = 0; i < 10_K; ++i)
    {
        auto pid = fast_pseudo_rand_int(0, ps.size() - 1);
        auto &p = ps[pid];
        if (!p->publish(i))
        {
            LOG(ERROR) << "** failed to publish at " << i;
            break;
        }

        for (size_t k = 0; k < i; ++k)
        {
            if (p->object(k))
            {
                CHECK_EQ(*(p->object(k)), k);
            }
        }
    }

    return 0;
}
