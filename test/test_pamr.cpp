#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>
#include <numa.h>

#include <thread>
#include <type_traits>

#include "GlobalAddress.h"
#include "HugePageAlloc.h"
#include "Rdma.h"
#include "Timer.h"
#include "avis/AvisAdaptor.h"
#include "avis/avis.h"
#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "jemalloc/jemalloc.h"
#include "jemalloc_cpp/jemalloc_cpp.h"
#include "memory/asan_interfaces.h"
#include "patronus/memory/direct_allocator.h"
#include "thirdparty/racehashing/hashtable.h"
#include "thirdparty/racehashing/race.h"
#include "util/Bitset.h"
#include "util/Hexdump.hpp"
#include "util/ProcessMem.h"
#include "util/Rand.h"
#include "util/System.h"
#include "util/Util.h"
#include "util/bits.h"
#include "util/concept.h"
#include "util/gflags_def.h"
#include "util/stacktrace.h"

// void test(DSM::pointer dsm)
// {
//     auto rdma_buf = dsm->get_rdma_buffer(32);
//     auto gaddr = dsm->alloc(32);
//     memset(rdma_buf.buffer, 0xff, 32);

//     dsm->prepare_write(rdma_buf.buffer, gaddr, 32, false, nullptr);
//     dsm->commit();

//     char add_val[32];
//     memset(add_val, 0xff, sizeof(add_val));
//     char field_boundary[32];
//     memset(field_boundary, 0xff, sizeof(field_boundary));
//     dsm->prepare_faa(gaddr,
//                      32,
//                      (uint64_t) add_val,
//                      (uint64_t) field_boundary,
//                      rdma_buf.buffer,
//                      false);
//     dsm->commit();
//     LOG(INFO) << "After FAA, rdma_buf: " << util::Hexdump(rdma_buf.buffer,
//     32); auto [nid, buf] = dsm->explain_gaddr(gaddr); CHECK_EQ(nid,
//     dsm->get_node_id()); LOG(INFO) << "After FAA, local_buf: " <<
//     util::Hexdump(buf, 32);
// }

// void cas_get(DSM::pointer dsm, GlobalAddress gaddr, size_t size)
// {
//     auto rdma_buf = dsm->get_rdma_buffer(size);
//     std::vector<char> compare(size, 0);
//     std::vector<char> swap(size, 0xff);
//     std::vector<char> mask(size, 0xff);

//     dsm->prepare_cas(gaddr,
//                      size,
//                      (uint64_t) compare.data(),
//                      (uint64_t) mask.data(),
//                      (uint64_t) swap.data(),
//                      (uint64_t) mask.data(),
//                      rdma_buf.buffer,
//                      false,
//                      nullptr);
//     dsm->commit();

//     LOG(INFO) << "[CAS] get_old: " << std::endl
//               << util::Bindump(rdma_buf.buffer, size);
//     uint64_t *tmp = (uint64_t *) malloc(size);
//     memcpy(tmp, rdma_buf.buffer, size);
//     for (size_t i = 0; i < size / sizeof(uint64_t); ++i)
//     {
//         tmp[i] = bswap64(tmp[i]);
//     }
//     LOG(INFO) << "[CAS] after htonll got: " << std::endl
//               << util::Bindump(tmp, size);
//     free(tmp);
//     dsm->put_rdma_buffer(std::move(rdma_buf));
// }

// void faa_get(DSM::pointer dsm, GlobalAddress gaddr, size_t size)
// {
//     auto rdma_buf = dsm->get_rdma_buffer(size);
//     std::vector<char> add_val(size, 0);
//     std::vector<char> boundary(size, 0xff);

//     dsm->prepare_faa(gaddr,
//                      size,
//                      (uint64_t) add_val.data(),
//                      (uint64_t) boundary.data(),
//                      rdma_buf.buffer,
//                      false,
//                      nullptr);
//     dsm->commit();

//     auto bs = util::BitsViewMut(rdma_buf.buffer, size);
//     LOG(INFO) << "[FAA] get_old: " << std::endl << bs.bin_dump();
//     bs.bswap64();
//     LOG(INFO) << "[FAA] get_old after bswap64: " << std::endl <<
//     bs.bin_dump();

//     dsm->put_rdma_buffer(std::move(rdma_buf));
// }

// extern "C"
// {
//     void __msan_poison(const volatile void *a, size_t size);
// }

// int *p = nullptr;

void local_pamr(DSM::pointer dsm)
{
    ibv_exp_reg_mr_in in{};
    auto *pd = dsm->get_th_pd();
    in.pd = pd;
    in.addr = nullptr;
    in.length = 0;
    // in.addr = huge_page;
    // in.length = 32_MB;
    in.exp_access = IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_REMOTE_WRITE |
                    IBV_EXP_ACCESS_REMOTE_READ | IBV_EXP_ACCESS_REMOTE_ATOMIC |
                    IBV_EXP_ACCESS_PHYSICAL_ADDR;
    // in.create_flags =

    auto *pmr = ibv_exp_reg_mr(&in);
    // while (true)
    // {
    // }
    LOG_IF(FATAL, pmr == nullptr) << "Failed to reg mr";

    LOG(INFO) << PRE(*pmr);

    char *page = (char *) hugePageAlloc(2_MB);
    memset(page, 'a', 2_MB);

    util::MemoryPVMapping mapping;
    // int a = 0;
    for (char *i = page; i < page + 16_KB; i += 4_KB)
    {
        void *pa = mapping.virt_to_phys(i);
        LOG(INFO) << "VA: " << (void *) i << ", pa: " << pa;
    }

    int a = 0;
    LOG(INFO) << "a: VA: " << (void *) &a
              << ", pa: " << mapping.virt_to_phys(&a);

    uint64_t base_addr = dsm->get_base_addr();
    auto dir_id = 0;
    auto rkey = dsm->get_base_rkey(dir_id);
    auto &qp = dsm->get_th_cqp(dsm->get_node_id(), dir_id);
    // auto *qp = dsm->get_th_qp(dsm->get_node_id(), dir_id);

    LOG(INFO) << "Before: " << std::endl
              << util::Hexdump((void *) base_addr, 32);
    {
        ibv_send_wr wr{};
        ibv_sge sge{};
        sge.addr = (uint64_t) mapping.virt_to_phys(page);
        sge.length = 32;
        sge.lkey = pmr->lkey;

        /* prepare the send work request */
        wr.next = NULL;
        wr.wr_id = 1;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.send_flags |= IBV_SEND_SIGNALED;
        wr.wr.rdma.remote_addr = base_addr;
        wr.wr.rdma.rkey = rkey;

        ibv_send_wr *bad_wr{};
        auto ret = ibv_post_send(qp.ibqp(), &wr, &bad_wr);
        PLOG_IF(FATAL, ret) << "Failed to ibv_post_send: " << PRE(*bad_wr);

        // qp.cq()->wait(1);
        ibv_wc wc{};
        auto *ibcq = qp.cq()->ibcq();
        pollWithCQ(ibcq, 1, &wc);
        LOG(INFO) << "OK. Let see" << std::endl << PRE(wc);
        LOG(INFO) << std::endl << util::Hexdump((void *) base_addr, 32);
    }
}

void remote_pamr(DSM::pointer dsm)
{
    ibv_exp_reg_mr_in in{};
    auto dir_id = 0;
    in.pd = dsm->get_dir_rdma_context(dir_id)->pd;
    in.addr = nullptr;
    in.length = 0;
    in.exp_access = IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_REMOTE_WRITE |
                    IBV_EXP_ACCESS_REMOTE_READ | IBV_EXP_ACCESS_REMOTE_ATOMIC |
                    IBV_EXP_ACCESS_PHYSICAL_ADDR;

    auto *pmr = ibv_exp_reg_mr(&in);

    LOG_IF(FATAL, pmr == nullptr) << "Failed to reg mr";

    LOG(INFO) << PRE(*pmr);

    util::MemoryPVMapping mapping;

    uint64_t base_addr = dsm->get_base_addr();
    auto &qp = dsm->get_th_cqp(dsm->get_node_id(), dir_id);
    memset((void *) base_addr, 0, 4_KB);
    LOG(INFO) << "Before: " << std::endl
              << util::Hexdump((void *) base_addr, 32);

    {
        auto rdma_buf = dsm->get_rdma_buffer(32);
        memset(rdma_buf.buffer, 'a', 32);
        ibv_send_wr wr{};
        ibv_sge sge{};
        sge.addr = (uint64_t) rdma_buf.buffer;
        sge.length = 32;
        sge.lkey = dsm->get_icon_lkey();

        /* prepare the send work request */
        wr.next = NULL;
        wr.wr_id = 1;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.send_flags |= IBV_SEND_SIGNALED;
        wr.wr.rdma.remote_addr =
            (uint64_t) mapping.virt_to_phys((void *) base_addr);
        // wr.wr.rdma.remote_addr = base_addr;
        wr.wr.rdma.rkey = pmr->rkey;
        // wr.wr.rdma.remote_addr = base_addr;
        // wr.wr.rdma.rkey = dsm->get_base_rkey(dir_id);

        ibv_send_wr *bad_wr{};
        LOG(INFO) << PRE(wr);
        auto ret = ibv_post_send(qp.ibqp(), &wr, &bad_wr);
        PLOG_IF(FATAL, ret) << "Failed to ibv_post_send: " << PRE(*bad_wr);

        // qp.cq()->wait(1);
        ibv_wc wc{};
        auto *ibcq = qp.cq()->ibcq();
        pollWithCQ(ibcq, 1, &wc);
        PLOG_IF(FATAL, wc.status != IBV_WC_SUCCESS) << "** failed: " << PRE(wc);
        LOG(INFO) << "OK. Let see" << std::endl << PRE(wc);
        LOG(INFO) << std::endl << util::Hexdump((void *) base_addr, 32);
    }
}

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig config;
    config.dsmSize = 2_GB;
    config.cacheConfig.cacheSize = 2_GB;
    config.worker_nr = 0;
    auto dsm = DSM::getInstance(config);
    dsm->registerThread();

    local_pamr(dsm);
    // remote_pamr(dsm);

    LOG(INFO) << "PASS.";
    return 0;
}