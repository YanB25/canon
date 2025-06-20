#include "GlobalAddress.h"
#include "HugePageAlloc.h"
#include "Timer.h"
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

void test(DSM::pointer dsm,
          size_t client_nr,
          GlobalAddress meta,
          size_t meta_size,
          CoroContext *ctx)
{
    std::vector<std::unique_ptr<avis::arc::ARC>> arcs;
    for (size_t i = 0; i < client_nr; ++i)
    {
        auto ptl =
            std::make_shared<avis::PTL>(dsm, dsm->getClusterSize() - 1, ctx);
        arcs.emplace_back(std::make_unique<avis::arc::ARC>(
            dsm, client_nr, i, meta, meta_size, ptl, ctx));
        arcs.back()->init();
    }

    auto ref = dsm->alloc(8);
    auto ref2 = dsm->alloc(8);
    auto ref3 = dsm->alloc(8);
    auto block = dsm->alloc(64 + sizeof(avis::arc::Header));
    auto block2 = dsm->alloc(64 + sizeof(avis::arc::Header));
    block = block + sizeof(avis::arc::Header);
    block2 = block2 + sizeof(avis::arc::Header);

    ChronoTimer timer;
    arcs[0]->AttachReference(ref, block);
    auto ns = timer.pin();
    LOG(INFO) << arcs[0]->metrics();
    LOG(INFO) << "Take " << util::pre_ns(ns);
    arcs[0]->report();
    arcs[0]->report(block);

    arcs[1]->AttachReference(ref2, block);
    arcs[0]->report();
    arcs[0]->report(block);

    arcs[2]->AttachReference(ref3, block);
    arcs[0]->report();
    arcs[0]->report(block);
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

    test(dsm, 4, meta, 2_MB, nullptr);

    return 0;
}
