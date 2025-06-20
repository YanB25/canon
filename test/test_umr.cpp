#include <malloc.h>
#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

#include "Rdma.h"
#include "bench/DataFrame.h"
#include "bench/experiment.h"
#include "bench/local_experiment.h"
#include "bench/storage.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "infiniband/verbs.h"
#include "infiniband/verbs_exp.h"
#include "mlx5/mlx5.h"
#include "util/Hexdump.hpp"
#include "util/History.h"
#include "util/Pre.h"
#include "util/ProcessMem.h"
#include "util/RingBuffer.h"
#include "util/gflags_def.h"
extern "C"
{
#include "infiniband/mlx5dv.h"
}
DEFINE_string(msg, "hello workd", "the message");
DEFINE_uint32(mr_nr, 3, "number of MR");
DEFINE_uint32(mr_size, 4_KB, "size of each MR");
DEFINE_uint64(base_addr, 0xffffffff00000000, "The base of MR");

using namespace mlx5;

void pollCQ(ibv_cq *cq, ibv_wc *wc, ssize_t remain)
{
    ssize_t polled_nr = 0;
    while (remain > 0)
    {
        int ret = ibv_poll_cq(cq, remain, wc + polled_nr);
        remain -= ret;
        polled_nr += ret;
    }
}
void pollCQ(ibv_cq *cq, ibv_exp_wc *wc, ssize_t remain)
{
    ssize_t polled_nr = 0;
    while (remain > 0)
    {
        int ret =
            ibv_exp_poll_cq(cq, remain, wc + polled_nr, sizeof(ibv_exp_wc));
        remain -= ret;
        polled_nr += ret;
    }
}

void rdma_write(ibv_qp *qp,
                uint64_t actual_gaddr,
                size_t size,
                const Buffer &rdma_buffer,
                uint32_t lkey,
                uint32_t rkey,
                uint64_t wr_id,
                bool signal)
{
    constexpr static size_t kSQE_Nr = 1;

    ibv_send_wr wr;
    memset(&wr, 0, sizeof(ibv_send_wr));

    ibv_sge sge[kSQE_Nr];
    memset(&sge, 0, sizeof(sge));

    for (size_t j = 0; j < kSQE_Nr; ++j)
    {
        sge[j].addr = (uint64_t) rdma_buffer.buffer;
        // LOG(INFO) << PRE((void *) rdma_buffer.buffer) << " with char "
        //           << (void *) (uint64_t) rdma_buffer.buffer[0];
        sge[j].length = size;
        sge[j].lkey = lkey;
    }

    /* prepare the send work request */
    wr.next = NULL;
    wr.wr_id = wr_id;
    wr.sg_list = sge;
    wr.num_sge = kSQE_Nr;
    wr.opcode = IBV_WR_RDMA_WRITE;
    // wr.imm_data = kImmData;
    if (signal)
    {
        wr.send_flags |= IBV_SEND_SIGNALED;
    }
    wr.wr.rdma.remote_addr = actual_gaddr;
    wr.wr.rdma.rkey = rkey;

    ibv_send_wr *bad_wr{};
    {
        // LOG(INFO) << PRE(wr);
        auto ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret)
        {
            PLOG(FATAL) << "Failed to ibv_post_send with " << PRE(qp) << ", "
                        << PRE(wr);
        }
    }
}

void rdma_read(ibv_qp *qp,
               uint64_t actual_gaddr,
               size_t size,
               const Buffer &rdma_buffer,
               uint32_t lkey,
               uint32_t rkey,
               uint64_t wr_id,
               bool signal)
{
    constexpr static size_t kSQE_Nr = 1;

    ibv_send_wr wr;
    memset(&wr, 0, sizeof(ibv_send_wr));

    ibv_sge sge[kSQE_Nr];
    memset(&sge, 0, sizeof(sge));

    for (size_t j = 0; j < kSQE_Nr; ++j)
    {
        sge[j].addr = (uint64_t) rdma_buffer.buffer;
        // LOG(INFO) << PRE((void *) rdma_buffer.buffer) << " with char "
        //           << (void *) (uint64_t) rdma_buffer.buffer[0];
        sge[j].length = size;
        sge[j].lkey = lkey;
    }

    /* prepare the send work request */
    wr.next = NULL;
    wr.wr_id = wr_id;
    wr.sg_list = sge;
    wr.num_sge = kSQE_Nr;
    wr.opcode = IBV_WR_RDMA_READ;
    // wr.imm_data = kImmData;
    if (signal)
    {
        wr.send_flags |= IBV_SEND_SIGNALED;
    }
    wr.wr.rdma.remote_addr = actual_gaddr;
    wr.wr.rdma.rkey = rkey;

    ibv_send_wr *bad_wr{};
    {
        // LOG(INFO) << PRE(wr);
        auto ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret)
        {
            PLOG(FATAL) << "Failed to ibv_post_send with " << PRE(qp) << ", "
                        << PRE(wr);
        }
    }
}

struct umr_context
{
    struct ibv_pd *pd;
    ibv_mr **mr_arr;
    struct ibv_mr *umr;
    struct ibv_exp_mkey_list_container *mkey_list_container;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
};

// https://github.com/microsoft/Freeflow/blob/master/libraries/libibverbs-1.2.1mlnx1/examples/umr_rc.c
ibv_mr *prepare_umr(ibv_pd *pd, size_t max_klm_list_size)
{
    struct ibv_exp_create_mr_in mrin;
    memset(&mrin, 0, sizeof(mrin));
    mrin.pd = pd;
    mrin.attr.create_flags = IBV_EXP_MR_INDIRECT_KLMS;
    mrin.attr.exp_access_flags =
        IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_REMOTE_WRITE |
        IBV_EXP_ACCESS_REMOTE_READ | IBV_EXP_ACCESS_REMOTE_ATOMIC;
    mrin.attr.max_klm_list_size = max_klm_list_size;
    ibv_mr *umr = ibv_exp_create_mr(&mrin);
    if (!umr)
    {
        PLOG(ERROR) << "Failed to create modified_mr";
        return nullptr;
    }
    return umr;
}

static void create_list_umr(ibv_mr *umr,
                            ibv_qp *qp,
                            const std::vector<ibv_mr *> &mrs,
                            uint64_t base_addr,
                            bool signal)
{
    std::vector<ibv_exp_mem_region> mem_reg_list(mrs.size());
    // struct ibv_exp_mem_region *mem_reg_list = NULL;
    struct ibv_exp_send_wr wr;
    struct ibv_exp_send_wr *bad_wr;
    int rc;

    // mem_reg_list = (ibv_exp_mem_region *) CHECK_NOTNULL(
    //     calloc(mrs.size(), sizeof(*mem_reg_list)));
    // mem_reg_list = &mem_reg_list_;

    size_t umr_len = 0;
    for (size_t i = 0; i < mrs.size(); i++)
    {
        mem_reg_list[i].base_addr = (uint64_t) (uintptr_t) mrs[i]->addr;
        mem_reg_list[i].length = mrs[i]->length;
        mem_reg_list[i].mr = mrs[i];
        umr_len += mem_reg_list[i].length;
    }

    memset(&wr, 0, sizeof(wr));
    wr.ext_op.umr.umr_type = IBV_EXP_UMR_MR_LIST;
    wr.ext_op.umr.mem_list.mem_reg_list = mem_reg_list.data();

    wr.exp_send_flags = IBV_EXP_SEND_INLINE;

    wr.ext_op.umr.exp_access =
        IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_REMOTE_WRITE |
        IBV_EXP_ACCESS_REMOTE_READ | IBV_EXP_ACCESS_REMOTE_ATOMIC;
    wr.ext_op.umr.modified_mr = umr;
    wr.ext_op.umr.base_addr = base_addr;
    wr.ext_op.umr.num_mrs = mrs.size();
    if (signal)
    {
        wr.exp_send_flags |= IBV_EXP_SEND_SIGNALED;
    }
    wr.exp_opcode = IBV_EXP_WR_UMR_FILL;

    // LOG(INFO) << PRE(wr);

    rc = ibv_exp_post_send(qp, &wr, &bad_wr);
    PLOG_IF(ERROR, rc != 0) << "Failed to ibv_post_send: " << *bad_wr;
    umr->length = umr_len;
}

void create_repeated_umr(ibv_mr *umr,
                         ibv_qp *qp,
                         std::vector<ibv_mr *> mrs,
                         uint64_t base_addr,
                         int rb_len,
                         int rb_stride,
                         int rb_count)
{
    struct ibv_exp_mem_repeat_block *mem_rep_list = NULL;
    struct ibv_exp_send_wr wr;
    struct ibv_exp_send_wr *bad_wr;
    int rc;
    int umr_len = 0;
    size_t ndim = 1;
    size_t *rpt_cnt = NULL;

    mem_rep_list = (ibv_exp_mem_repeat_block *) CHECK_NOTNULL(
        calloc(mrs.size(), sizeof(*mem_rep_list)));

    for (size_t i = 0; i < mrs.size(); i++)
    {
        mem_rep_list[i].byte_count = (size_t *) CHECK_NOTNULL(
            calloc(ndim, sizeof(mem_rep_list[i].byte_count[0])));

        mem_rep_list[i].stride = (size_t *) CHECK_NOTNULL(
            calloc(ndim, sizeof(mem_rep_list[i].stride[0])));
    }

    rpt_cnt = (size_t *) CHECK_NOTNULL(calloc(ndim, sizeof(*rpt_cnt)));

    for (size_t i = 0; i < ndim; i++)
    {
        rpt_cnt[i] = rb_count;
    }

    for (size_t i = 0; i < mrs.size(); i++)
    {
        mem_rep_list[i].base_addr = (uint64_t) (uintptr_t) mrs[i]->addr;
        mem_rep_list[i].byte_count[0] = rb_len;
        mem_rep_list[i].mr = mrs[i];
        mem_rep_list[i].stride[0] = rb_stride;

        umr_len += rb_count * mem_rep_list[i].byte_count[0];
    }

    memset(&wr, 0, sizeof(wr));
    wr.ext_op.umr.umr_type = IBV_EXP_UMR_REPEAT;
    wr.ext_op.umr.mem_list.rb.mem_repeat_block_list = mem_rep_list;
    wr.ext_op.umr.mem_list.rb.stride_dim = 1;
    wr.ext_op.umr.mem_list.rb.repeat_count = rpt_cnt;

    wr.exp_send_flags = IBV_EXP_SEND_INLINE;

    wr.ext_op.umr.exp_access =
        IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_REMOTE_WRITE |
        IBV_EXP_ACCESS_REMOTE_READ | IBV_EXP_ACCESS_REMOTE_ATOMIC;
    wr.ext_op.umr.modified_mr = umr;
    wr.ext_op.umr.base_addr = base_addr;
    wr.ext_op.umr.num_mrs = mrs.size();
    wr.exp_send_flags |= IBV_EXP_SEND_SIGNALED;
    wr.exp_opcode = IBV_EXP_WR_UMR_FILL;

    rc = ibv_exp_post_send(qp, &wr, &bad_wr);
    PLOG_IF(FATAL, rc) << "** failed to ibv_exp_post_send: " << PRE(wr);
    umr->length = umr_len;
}

void dump(ibv_mr *mr)
{
    char first = *((char *) mr->addr);
    char last = *((char *) mr->addr + mr->length - 1);
    LOG(INFO) << "MR " << (void *) mr << ": addr: " << mr->addr
              << ", length: " << mr->length << ", " << PRE(first) << ", "
              << PRE(last);
}

void test_list_umr(DSM::pointer dsm)
{
    auto to_nid = dsm->get_node_id();
    auto to_tid = dsm->get_thread_id();
    auto dir_id = 0;
    // auto iqp = dsm->get_dir_iqp(to_nid, to_tid, dir_id);
    auto *qp = dsm->get_dir_qp(to_nid, to_tid, dir_id);
    auto *cq = dsm->get_dir_cq(dir_id);
    auto &ctx = *dsm->get_dir_rdma_context(dir_id);

    umr_context umr_ctx;
    umr_ctx.pd = ctx.pd;
    umr_ctx.mr_arr = (ibv_mr **) malloc(sizeof(ibv_mr *) * FLAGS_mr_nr);
    std::vector<ibv_mr *> mrs;
    for (size_t i = 0; i < FLAGS_mr_nr; ++i)
    {
        auto *addr = CHECK_NOTNULL(memalign(2_MB, FLAGS_mr_size));

        ibv_mr *mr =
            ibv_reg_mr(ctx.pd,
                       addr,
                       4_KB,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ);
        mrs.push_back(mr);
    }
    umr_ctx.qp = qp;
    umr_ctx.cq = cq;

    uint64_t base_addr = FLAGS_base_addr;

    ibv_mr *umr = prepare_umr(ctx.pd, mrs.size());
    create_list_umr(umr, qp, mrs, base_addr, true);
    ibv_wc wc;
    pollCQ(cq, &wc, 1);
    LOG(INFO) << PRE(wc);
    LOG(INFO) << PRE(*umr);

    for (auto *mr : mrs)
    {
        dump(mr);
    }

    {
        auto to_nid = dsm->get_node_id();
        auto to_dir = 0;
        auto qp = dsm->get_th_qp(to_nid, to_dir);
        auto *ibcq = dsm->get_icon_ibcq();
        uint64_t remote_addr = base_addr;
        size_t size = FLAGS_mr_nr * (FLAGS_mr_size - 1024);
        auto rdma_buf = dsm->get_rdma_buffer(size);
        memset(rdma_buf.buffer, 'a', size);
        auto lkey = dsm->get_icon_lkey();
        auto rkey = umr->rkey;
        rdma_write(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
        ibv_wc wc;
        pollCQ(ibcq, &wc, 1);
        LOG(INFO) << PRE(wc);
        for (auto *mr : mrs)
        {
            dump(mr);
        }
    }

    int ret = ibv_dereg_mr(umr);
    PLOG_IF(FATAL, ret) << "failed to dereg mr";
    for (ibv_mr *mr : mrs)
    {
        int ret = ibv_dereg_mr(mr);
        PLOG_IF(FATAL, ret) << "failed to dereg mr";
    }
}

void test_list_umr_over_pamr(DSM::pointer dsm)
{
    auto to_nid = dsm->get_node_id();
    auto to_tid = dsm->get_thread_id();
    auto dir_id = 0;
    // auto iqp = dsm->get_dir_iqp(to_nid, to_tid, dir_id);
    auto *qp = dsm->get_dir_qp(to_nid, to_tid, dir_id);
    auto *cq = dsm->get_dir_cq(dir_id);
    auto &ctx = *dsm->get_dir_rdma_context(dir_id);

    umr_context umr_ctx;
    umr_ctx.pd = ctx.pd;
    umr_ctx.mr_arr = (ibv_mr **) malloc(sizeof(ibv_mr *));
    std::vector<ibv_mr *> mrs;

    ibv_exp_reg_mr_in in{};
    in.pd = ctx.pd;
    in.addr = nullptr;
    in.length = 0;
    in.exp_access = IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_REMOTE_WRITE |
                    IBV_EXP_ACCESS_REMOTE_READ | IBV_EXP_ACCESS_REMOTE_ATOMIC |
                    IBV_EXP_ACCESS_PHYSICAL_ADDR;

    util::MemoryPVMapping mapping;

    auto *pamr = ibv_exp_reg_mr(&in);
    pamr->length = 32_MB;
    PLOG_IF(FATAL, pamr == nullptr) << "** failed to reg mr";
    LOG(INFO) << PRE(*pamr);
    mrs.push_back(pamr);

    umr_ctx.qp = qp;
    umr_ctx.cq = cq;

    // uint64_t base_addr = FLAGS_base_addr;
    // uint64_t base_addr = 0;
    uint64_t remote_base = dsm->get_base_addr();
    uint64_t phy_remote_base =
        (uint64_t) mapping.virt_to_phys((void *) remote_base);

    ibv_mr *umr = prepare_umr(ctx.pd, mrs.size());
    {
        std::vector<ibv_exp_mem_region> mem_reg_list(mrs.size());
        // struct ibv_exp_mem_region *mem_reg_list = NULL;
        struct ibv_exp_send_wr wr;
        struct ibv_exp_send_wr *bad_wr;
        int rc;

        size_t umr_len = 0;
        for (size_t i = 0; i < mrs.size(); i++)
        {
            // mem_reg_list[i].base_addr = (uint64_t) (uintptr_t) mrs[i]->addr;
            // mem_reg_list[i].base_addr = 0;
            mem_reg_list[i].base_addr = phy_remote_base;
            // mem_reg_list[i].length = mrs[i]->length;
            // mem_reg_list[i].length = 32_MB;
            mem_reg_list[i].length = -1;
            // mem_reg_list[i].length = 0;
            mem_reg_list[i].mr = mrs[i];
            umr_len += mem_reg_list[i].length;
        }

        memset(&wr, 0, sizeof(wr));
        wr.ext_op.umr.umr_type = IBV_EXP_UMR_MR_LIST;
        wr.ext_op.umr.mem_list.mem_reg_list = mem_reg_list.data();
        wr.ext_op.umr.num_mrs = mrs.size();

        wr.exp_send_flags = IBV_EXP_SEND_INLINE;

        wr.ext_op.umr.exp_access =
            IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_REMOTE_WRITE |
            IBV_EXP_ACCESS_REMOTE_READ | IBV_EXP_ACCESS_REMOTE_ATOMIC;
        wr.ext_op.umr.modified_mr = umr;
        // wr.ext_op.umr.base_addr = phy_remote_base;
        wr.ext_op.umr.base_addr = 0;
        // wr.ext_op.umr.num_mrs = mrs.size();
        wr.exp_send_flags |= IBV_EXP_SEND_SIGNALED;

        wr.exp_opcode = IBV_EXP_WR_UMR_FILL;

        LOG(INFO) << PRE(wr);

        rc = ibv_exp_post_send(qp, &wr, &bad_wr);
        PLOG_IF(ERROR, rc != 0) << "Failed to ibv_post_send: " << *bad_wr;
        umr->length = umr_len;
    }
    ibv_wc wc;
    pollCQ(cq, &wc, 1);
    PLOG_IF(FATAL, wc.status != IBV_WC_SUCCESS)
        << "Failed to create UMR: " << PRE(wc);
    LOG(INFO) << PRE(wc);
    LOG(INFO) << PRE(*umr);

    // for (auto *mr : mrs)
    // {
    //     dump(mr);
    // }

    {
        auto to_nid = dsm->get_node_id();
        auto to_dir = 0;
        auto qp = dsm->get_th_qp(to_nid, to_dir);
        auto *ibcq = dsm->get_icon_ibcq();
        // uint64_t remote_addr = phy_remote_base;
        uint64_t remote_addr = 0;
        size_t size = 32;
        auto rdma_buf = dsm->get_rdma_buffer(size);
        memset(rdma_buf.buffer, 'a', size);
        auto lkey = dsm->get_icon_lkey();
        auto rkey = umr->rkey;
        LOG(ERROR) << "!!!! Going to issue RDMA write.";
        rdma_write(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
        ibv_wc wc;
        pollCQ(ibcq, &wc, 1);
        LOG(INFO) << PRE(wc);
        LOG(INFO) << std::endl << util::Hexdump((void *) remote_base, 32);
    }

    int ret = ibv_dereg_mr(umr);
    PLOG_IF(FATAL, ret) << "failed to dereg mr";
    for (ibv_mr *mr : mrs)
    {
        int ret = ibv_dereg_mr(mr);
        PLOG_IF(FATAL, ret) << "failed to dereg mr";
    }
}

void test_repeated_umr(DSM::pointer dsm)
{
    auto to_nid = dsm->get_node_id();
    auto to_tid = dsm->get_thread_id();
    auto dir_id = 0;
    auto *qp = dsm->get_dir_qp(to_nid, to_tid, dir_id);
    auto *cq = dsm->get_dir_cq(dir_id);
    auto &ctx = *dsm->get_dir_rdma_context(dir_id);

    umr_context umr_ctx;
    umr_ctx.pd = ctx.pd;
    umr_ctx.mr_arr = (ibv_mr **) malloc(sizeof(ibv_mr *) * FLAGS_mr_nr);
    std::vector<ibv_mr *> mrs;
    for (size_t i = 0; i < FLAGS_mr_nr; ++i)
    {
        auto *addr = CHECK_NOTNULL(memalign(2_MB, FLAGS_mr_size));

        ibv_mr *mr =
            ibv_reg_mr(ctx.pd,
                       addr,
                       4_KB,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ);
        mrs.push_back(mr);
    }
    umr_ctx.qp = qp;
    umr_ctx.cq = cq;

    uint64_t base_addr = FLAGS_base_addr;

    // ibv_mr *umr = create_list_umr(ctx.pd, iqp.ibqp(), mrs, base_addr, true);
    ibv_mr *umr = prepare_umr(ctx.pd, mrs.size());
    create_repeated_umr(umr, qp, mrs, base_addr, 32, 64, FLAGS_mr_size / 64);
    ibv_wc wc;
    pollCQ(cq, &wc, 1);
    LOG(INFO) << PRE(wc);
    LOG(INFO) << PRE(*umr);

    // LOG(INFO) << util::Hexdump(mrs[0]->addr, 512);

    {
        auto to_nid = dsm->get_node_id();
        auto to_dir = 0;
        auto *qp = dsm->get_th_qp(to_nid, to_dir);
        auto *cq = dsm->get_icon_ibcq();
        uint64_t remote_addr = base_addr;
        size_t size = FLAGS_mr_size;
        auto rdma_buf = dsm->get_rdma_buffer(size);
        memset(rdma_buf.buffer, 'a', size);
        auto lkey = dsm->get_icon_lkey();
        auto rkey = umr->rkey;
        rdma_write(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
        ibv_wc wc;
        pollCQ(cq, &wc, 1);
        LOG(INFO) << PRE(wc);

        // LOG(INFO) << util::Hexdump(mrs[0]->addr, 512);
    }

    int ret = ibv_dereg_mr(umr);
    PLOG_IF(FATAL, ret) << "failed to dereg mr";
    for (ibv_mr *mr : mrs)
    {
        int ret = ibv_dereg_mr(mr);
        PLOG_IF(FATAL, ret) << "failed to dereg mr";
    }
}

void test_remap_umr(DSM::pointer dsm)
{
    auto to_nid = dsm->get_node_id();
    auto to_tid = dsm->get_thread_id();
    auto dir_id = 0;
    auto *qp = dsm->get_dir_qp(to_nid, to_tid, dir_id);
    auto *cq = dsm->get_dir_cq(dir_id);
    auto &ctx = *dsm->get_dir_rdma_context(dir_id);

    std::vector<ibv_mr *> mrs;
    for (size_t i = 0; i < FLAGS_mr_nr; ++i)
    {
        auto *addr = CHECK_NOTNULL(memalign(2_MB, FLAGS_mr_size));

        ibv_mr *mr =
            ibv_reg_mr(ctx.pd,
                       addr,
                       4_KB,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ);
        mrs.push_back(mr);
    }

    uint64_t base_addr = FLAGS_base_addr;

    ibv_mr *umr = prepare_umr(ctx.pd, mrs.size());
    create_repeated_umr(umr, qp, mrs, base_addr, 32, 64, FLAGS_mr_size / 64);
    ibv_wc wc;
    pollCQ(cq, &wc, 1);
    LOG(INFO) << PRE(wc);
    LOG(INFO) << PRE(*umr);

    LOG(INFO) << util::Hexdump(mrs[0]->addr, 512);

    {
        auto to_nid = dsm->get_node_id();
        auto to_dir = 0;
        auto *qp = dsm->get_th_qp(to_nid, to_dir);
        auto *cq = dsm->get_icon_ibcq();
        uint64_t remote_addr = base_addr;
        size_t size = FLAGS_mr_size;
        auto rdma_buf = dsm->get_rdma_buffer(size);
        memset(rdma_buf.buffer, 'a', size);
        auto lkey = dsm->get_icon_lkey();
        auto rkey = umr->rkey;
        rdma_write(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
        ibv_wc wc;
        pollCQ(cq, &wc, 1);
        LOG(INFO) << PRE(wc);

        LOG(INFO) << util::Hexdump(mrs[0]->addr, 512);
    }

    LOG(INFO) << "[system] now remap UMR...";
    create_repeated_umr(umr, qp, mrs, base_addr, 16, 64, FLAGS_mr_size / 64);
    pollCQ(cq, &wc, 1);
    LOG(INFO) << PRE(wc);
    LOG(INFO) << PRE(*umr);

    {
        auto to_nid = dsm->get_node_id();
        auto to_dir = 0;
        auto *qp = dsm->get_th_qp(to_nid, to_dir);
        auto *cq = dsm->get_icon_ibcq();
        uint64_t remote_addr = base_addr;
        size_t size = FLAGS_mr_size / 2;
        auto rdma_buf = dsm->get_rdma_buffer(size);
        memset(rdma_buf.buffer, 'b', size);
        auto lkey = dsm->get_icon_lkey();
        auto rkey = umr->rkey;
        rdma_write(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
        ibv_wc wc;
        pollCQ(cq, &wc, 1);
        LOG(INFO) << PRE(wc);

        LOG(INFO) << util::Hexdump(mrs[0]->addr, 512);
    }

    int ret = ibv_dereg_mr(umr);
    PLOG_IF(FATAL, ret) << "failed to dereg mr";
    for (ibv_mr *mr : mrs)
    {
        int ret = ibv_dereg_mr(mr);
        PLOG_IF(FATAL, ret) << "failed to dereg mr";
    }
}

struct WRCtx
{
    Buffer rdma_buf;
};

void test_keep_remapping_umr(DSM::pointer dsm)
{
    auto to_nid = dsm->get_node_id();
    auto to_tid = dsm->get_thread_id();
    auto dir_id = 0;
    auto *qp = dsm->get_dir_qp(to_nid, to_tid, dir_id);
    auto *cq = dsm->get_dir_cq(dir_id);
    auto &ctx = *dsm->get_dir_rdma_context(dir_id);

    std::vector<ibv_mr *> mrs;
    for (size_t i = 0; i < FLAGS_mr_nr; ++i)
    {
        auto *addr = CHECK_NOTNULL(memalign(2_MB, FLAGS_mr_size));

        ibv_mr *mr =
            ibv_reg_mr(ctx.pd,
                       addr,
                       FLAGS_mr_size,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ);
        mrs.push_back(mr);
        for (size_t i = 0; i < FLAGS_mr_size; ++i)
        {
            ((char *) addr)[i] = 'a' + (i % 26);
        }
    }

    uint64_t base_addr = FLAGS_base_addr;

    ibv_mr *umr = prepare_umr(ctx.pd, mrs.size());
    create_repeated_umr(umr, qp, mrs, base_addr, 16, 64, FLAGS_mr_size / 64);
    ibv_wc wc;
    pollCQ(cq, &wc, 1);
    LOG(INFO) << PRE(wc);
    LOG(INFO) << PRE(*umr);

    LOG(INFO) << util::Hexdump(mrs[0]->addr, 64);

    {
        auto to_nid = dsm->get_node_id();
        auto to_dir = 0;
        auto *qp = dsm->get_th_qp(to_nid, to_dir);
        auto *cq = dsm->get_icon_ibcq();
        uint64_t remote_addr = base_addr;
        size_t size = FLAGS_mr_size / 16;
        auto rdma_buf = dsm->get_rdma_buffer(size);
        memset(rdma_buf.buffer, 'a', size);
        auto lkey = dsm->get_icon_lkey();
        auto rkey = umr->rkey;
        rdma_read(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
        ibv_wc wc;
        pollCQ(cq, &wc, 1);
        LOG(INFO) << PRE(wc);

        LOG(INFO) << util::Hexdump(rdma_buf.buffer, 128);
        // LOG(INFO) << util::Hexdump(mrs[0]->addr, 128);
    }

    create_repeated_umr(umr, qp, mrs, base_addr, 32, 64, FLAGS_mr_size / 64);
    pollCQ(cq, &wc, 1);
    LOG(INFO) << PRE(wc);
    LOG(INFO) << PRE(*umr);

    {
        auto to_nid = dsm->get_node_id();
        auto to_dir = 0;
        auto qp = dsm->get_th_qp(to_nid, to_dir);
        auto *cq = dsm->get_icon_ibcq();
        uint64_t remote_addr = base_addr;
        size_t size = FLAGS_mr_size / 16;
        auto rdma_buf = dsm->get_rdma_buffer(size);
        memset(rdma_buf.buffer, 'a', size);
        auto lkey = dsm->get_icon_lkey();
        auto rkey = umr->rkey;
        rdma_read(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
        ibv_wc wc;
        pollCQ(cq, &wc, 1);
        LOG(INFO) << PRE(wc);

        // LOG(INFO) << util::Hexdump(mrs[0]->addr, 128);
        LOG(INFO) << util::Hexdump(rdma_buf.buffer, 128);
    }

    std::atomic<bool> stop{false};
    std::thread remap_thread(
        [&]()
        {
            auto max = util::time::to_ns(1ms);
            auto min = util::time::to_ns(0ns);
            auto rng = util::time::to_ns(1us);
            OnePassBucketMonitor m(min, max, rng);
            ChronoTimer timer;
            while (!stop.load())
            {
                timer.pin();
                create_repeated_umr(
                    umr, qp, mrs, base_addr, 16, 64, FLAGS_mr_size / 64);
                pollCQ(cq, &wc, 1);
                auto ns = timer.pin();
                m.collect(ns);

                timer.pin();
                create_repeated_umr(
                    umr, qp, mrs, base_addr, 32, 64, FLAGS_mr_size / 64);
                pollCQ(cq, &wc, 1);
                ns = timer.pin();
                m.collect(ns);
            }
            LOG(INFO) << PRE(m);
            LOG(INFO) << "exiting...";
        });

    auto now = std::chrono::steady_clock::now();
    size_t second = 0;
    ssize_t credit = 8;
    while (true)
    {
        while (credit)
        {
            auto *ctx = new WRCtx;
            size_t size = FLAGS_mr_size / 16;
            ctx->rdma_buf = dsm->get_rdma_buffer(size);

            credit--;
            auto to_nid = dsm->get_node_id();
            auto to_dir = 0;
            auto *qp = dsm->get_th_qp(to_nid, to_dir);
            uint64_t remote_addr = base_addr;
            memset(ctx->rdma_buf.buffer, 'b', size);
            auto lkey = dsm->get_icon_lkey();
            auto rkey = umr->rkey;
            // rdma_read(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
            ibv_send_wr wr{};
            ibv_sge sge{};
            sge.addr = (uintptr_t) ctx->rdma_buf.buffer;
            sge.length = size;
            sge.lkey = lkey;
            wr.next = nullptr;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.opcode = IBV_WR_RDMA_READ;
            wr.send_flags |= IBV_SEND_SIGNALED;
            wr.wr.rdma.remote_addr = remote_addr;
            wr.wr.rdma.rkey = rkey;

            wr.wr_id = (uintptr_t) ctx;

            ibv_send_wr *bad_wr{};
            int ret = ibv_post_send(qp, &wr, &bad_wr);
            PLOG_IF(FATAL, ret != 0) << PRE(*bad_wr);
        }
        ibv_wc wcs[64];
        auto *cq = dsm->get_icon_ibcq();
        int cnt = ibv_poll_cq(cq, 64, wcs);
        if (cnt)
        {
            credit += cnt;
            for (int i = 0; i < cnt; ++i)
            {
                auto &wc = wcs[i];
                if (unlikely(wc.status != IBV_WC_SUCCESS))
                {
                    PLOG(FATAL) << PRE(wc);
                }
                else
                {
                    WRCtx *ctx = (WRCtx *) wc.wr_id;
                    LOG_EVERY_N(INFO, (int) 100_K)
                        << util::Hexdump(ctx->rdma_buf.buffer, 64);
                    dsm->put_rdma_buffer(std::move(ctx->rdma_buf));
                    delete ctx;
                }
            }
        }

        auto then = std::chrono::steady_clock::now();
        if (then - now >= 100ms)
        {
            now = then;
            second++;
            if (second >= 40)
            {
                LOG(INFO) << "[master] exit";
                stop = true;
                break;
            }
        }
    }

    remap_thread.join();

    int ret = ibv_dereg_mr(umr);
    PLOG_IF(FATAL, ret) << "failed to dereg mr";
    for (ibv_mr *mr : mrs)
    {
        int ret = ibv_dereg_mr(mr);
        PLOG_IF(FATAL, ret) << "failed to dereg mr";
    }
}

void test_umr_over_dm(DSM::pointer dsm)
{
    auto to_nid = dsm->get_node_id();
    auto to_tid = dsm->get_thread_id();
    auto dir_id = 0;
    auto *qp = dsm->get_dir_qp(to_nid, to_tid, dir_id);
    auto *cq = dsm->get_dir_cq(dir_id);
    auto &ctx = *dsm->get_dir_rdma_context(dir_id);

    std::vector<ibv_mr *> mrs;
    auto *dir_mr = dsm->get_dir_mr(dir_id);
    auto *dir_dm_mr = dsm->get_dir_dm_mr();
    dir_mr->length = 64;
    dir_dm_mr->length = 64;
    mrs.push_back(dir_mr);
    mrs.push_back(dir_dm_mr);

    uint64_t base_addr = FLAGS_base_addr;

    ibv_mr *umr = prepare_umr(ctx.pd, mrs.size());
    create_list_umr(umr, qp, mrs, base_addr, true);
    ibv_wc wc;
    pollCQ(cq, &wc, 1);
    LOG(INFO) << PRE(wc);

    {
        LOG(INFO) << "Before";
        LOG(INFO) << util::Hexdump(dir_mr->addr, 128);
        auto *dm_buf = (char *) malloc(128);
        ibv_exp_memcpy_dm_attr dm_attr{.memcpy_dir = IBV_EXP_DM_CPY_TO_HOST,
                                       .host_addr = dm_buf,
                                       .dm_offset = 0,
                                       .length = 128};
        auto *dm = dsm->get_dir_dm();
        int ret = ibv_exp_memcpy_dm(dm, &dm_attr);
        PLOG_IF(FATAL, ret != 0) << "** failed to memcpy_dm";
        LOG(INFO) << util::Hexdump(dm_buf, 128);
    }

    {
        auto to_nid = dsm->get_node_id();
        auto to_dir = 0;
        auto qp = dsm->get_th_qp(to_nid, to_dir);
        auto *cq = dsm->get_icon_ibcq();
        uint64_t remote_addr = base_addr;
        size_t size = 128;
        auto rdma_buf = dsm->get_rdma_buffer(size);
        memset(rdma_buf.buffer, 'a', size);
        auto lkey = dsm->get_icon_lkey();
        auto rkey = umr->rkey;
        rdma_write(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
        ibv_wc wc;
        pollCQ(cq, &wc, 1);
        LOG(INFO) << PRE(wc);

        LOG(INFO) << util::Hexdump(dir_mr->addr, 128);

        auto *dm_buf = (char *) malloc(128);
        ibv_exp_memcpy_dm_attr dm_attr{.memcpy_dir = IBV_EXP_DM_CPY_TO_HOST,
                                       .host_addr = dm_buf,
                                       .dm_offset = 0,
                                       .length = 128};
        auto *dm = dsm->get_dir_dm();
        int ret = ibv_exp_memcpy_dm(dm, &dm_attr);
        PLOG_IF(FATAL, ret != 0) << "** failed to memcpy_dm";
        LOG(INFO) << util::Hexdump(dm_buf, 128);
    }

    int ret = ibv_dereg_mr(umr);
    PLOG_IF(FATAL, ret) << "failed to dereg mr";
}

bool test_umr_over_umr_level(DSM::pointer dsm, size_t l)
{
    bool ret = true;
    auto to_nid = dsm->get_node_id();
    auto to_tid = dsm->get_thread_id();
    auto dir_id = 0;
    auto *qp = dsm->get_dir_qp(to_nid, to_tid, dir_id);
    auto *cq = dsm->get_dir_cq(dir_id);
    auto &ctx = *dsm->get_dir_rdma_context(dir_id);

    auto *base_mr = dsm->get_dir_mr(dir_id);

    // IMPORTANT: this value must be aligned with base_mr->addr
    // to make recursive indirection work.
    // You CAN change base_mr->addr AND base_addr to another same value, also
    // okay.
    uint64_t base_addr = (uint64_t) base_mr->addr;

    LOG(INFO) << PRE(*base_mr);

    std::vector<ibv_mr *> umrs;

    for (size_t i = 0; i < l; ++i)
    {
        LOG(INFO) << "Creating " << i << " out of " << l;
        auto *this_umr = prepare_umr(ctx.pd, 1);
        LOG(INFO) << "prepare_umr: " << this_umr;
        std::vector<ibv_mr *> mr_list;
        if (umrs.empty())
        {
            mr_list.push_back(base_mr);
        }
        else
        {
            auto *last_umr = umrs.back();
            mr_list.push_back(last_umr);
        }

        LOG(INFO) << PRE(mr_list);
        for (auto *mr : mr_list)
        {
            LOG(INFO) << "In mr_list: " << PRE(*mr);
        }

        create_list_umr(this_umr, qp, mr_list, base_addr, true);
        // create_repeated_umr(this_umr,
        //                     qp,
        //                     mr_list,
        //                     base_addr,
        //                     64,
        //                     64,
        //                     mr_list.front()->length / 64);

        this_umr->length = base_mr->length;
        this_umr->addr = base_mr->addr;

        ibv_exp_wc wc;
        pollCQ(cq, &wc, 1);
        if (wc.status != IBV_WC_SUCCESS)
        {
            LOG(FATAL) << PRE(wc);
        }
        else
        {
            LOG(INFO) << PRE(wc);
        }
        umrs.push_back(this_umr);
    }

    // LOG(INFO) << PRE(umrs);
    for (auto *umr : umrs)
    {
        LOG(INFO) << PRE(*umr);
    }

    // for (auto *umr : umrs)
    {
        auto *umr = umrs.back();
        LOG(INFO) << PRE(*umr);

        auto to_nid = dsm->get_node_id();
        auto to_dir = 0;
        auto qp = dsm->get_th_qp(to_nid, to_dir);
        auto *cq = dsm->get_icon_ibcq();
        uint64_t remote_addr = base_addr;
        size_t size = 64;
        auto rdma_buf = dsm->get_rdma_buffer(size);
        memset(rdma_buf.buffer, 'a', size);
        auto lkey = dsm->get_icon_lkey();
        auto rkey = umr->rkey;
        rdma_write(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
        ibv_exp_wc wc;
        pollCQ(cq, &wc, 1);
        if (wc.status != IBV_WC_SUCCESS)
        {
            LOG(ERROR) << PRE(wc);
            ret = false;
        }
        else
        {
            LOG(INFO) << PRE(wc);
        }

        LOG(INFO) << util::Hexdump(base_mr->addr, 128);
    }

    LOG(INFO) << "OK, destructing...";
    for (auto *mr : umrs)
    {
        ibv_dereg_mr(mr);
    }

    return ret;
}

void test_umr_over_umr(DSM::pointer dsm)
{
    for (size_t i = 1; i < 8; ++i)
    {
        LOG(INFO) << "============== testing level " << i
                  << "=================";
        bool succ = test_umr_over_umr_level(dsm, i);
        if (!succ)
        {
            LOG(INFO) << "Level " << i
                      << " not supported: maximun support till level "
                      << (i - 1);
            break;
        }
    }
}

// void test_umr_over_mw(DSM::pointer dsm)
// {
//     auto to_nid = dsm->get_node_id();
//     auto to_tid = dsm->get_thread_id();
//     auto dir_id = 0;
//     auto *qp = dsm->get_dir_qp(to_nid, to_tid, dir_id);
//     auto *cq = dsm->get_dir_cq(dir_id);
//     auto &ctx = *dsm->get_dir_rdma_context(dir_id);

//     auto *mr = dsm->get_dir_mr(dir_id);

//     ibv_mw_bind mw_bind{
//         .wr_id = 0,
//         .send_flags = IBV_EXP_SEND_SIGNALED,
//         .bind_info = {.mr = mr,
//                       .addr = (uint64_t) mr->addr,
//                       .length = 128,
//                       .mw_access_flags = IBV_ACCESS_LOCAL_WRITE |
//                                          IBV_ACCESS_REMOTE_WRITE |
//                                          IBV_ACCESS_REMOTE_READ}};
//     auto *mw = CHECK_NOTNULL(ibv_alloc_mw(ctx.pd, IBV_MW_TYPE_1));
//     int ret = ibv_bind_mw(qp, mw, &mw_bind);
//     PLOG_IF(FATAL, ret) << "** failed to ibv_bind_mw";

//     LOG(INFO) << "MW ready, now bind UMR";

//     ibv_mr *umr = prepare_umr(ctx.pd, 1);
//     std::vector<ibv_mr*> mr_list;
//     auto* fake_mr = (ibv_mr*) malloc(sizeof(ibv_mr));
//     fake_mr->addr
//     // mr_list.push_back
// }

void test_umr_tree_simple(DSM::pointer dsm)
{
    auto to_nid = dsm->get_node_id();
    auto to_tid = dsm->get_thread_id();
    auto dir_id = 0;
    auto *qp = dsm->get_dir_qp(to_nid, to_tid, dir_id);
    auto *cq = dsm->get_dir_cq(dir_id);
    auto &ctx = *dsm->get_dir_rdma_context(dir_id);

    uint32_t access_flags = access_flags = IBV_ACCESS_LOCAL_WRITE |
                                           IBV_ACCESS_REMOTE_WRITE |
                                           IBV_ACCESS_REMOTE_READ;

    // last level: 16
    std::vector<ibv_mr *> l1_mrs;
    for (size_t i = 0; i < 16; ++i)
    {
        auto *addr = malloc(4_KB);
        auto *mr = ibv_reg_mr(ctx.pd, addr, 4_KB, access_flags);
        l1_mrs.push_back(CHECK_NOTNULL(mr));
    }

    // construct 4 l1 umr, each with 4 l1 mr
    std::vector<ibv_mr *> l1_umrs;
    for (size_t i = 0; i < 4; ++i)
    {
        auto *umr = prepare_umr(ctx.pd, 4);
        size_t start_mr_idx = i * 4;
        size_t end_mr_idx = start_mr_idx + 4;
        std::vector<ibv_mr *> connect_mrs;
        for (size_t b = start_mr_idx; b < end_mr_idx; ++b)
        {
            connect_mrs.push_back(l1_mrs[b]);
        }
        auto base_addr = connect_mrs.front()->addr;
        create_list_umr(umr, qp, connect_mrs, (uint64_t) base_addr, true);
        ibv_exp_wc wc;
        pollCQ(cq, &wc, 1);
        if (wc.status == IBV_WC_SUCCESS)
        {
            LOG(INFO) << PRE(wc);
        }
        else
        {
            LOG(FATAL) << PRE(wc);
        }

        umr->addr = base_addr;
        // should be sum of all connect_mrs->length.
        // but for convenience, just hard code it.
        umr->length = 16_KB;
        l1_umrs.push_back(umr);
    }

    // now, top level
    auto *top_umr = prepare_umr(ctx.pd, 4);
    create_list_umr(
        top_umr, qp, l1_umrs, (uint64_t) l1_umrs.front()->addr, true);
    ibv_exp_wc wc;
    pollCQ(cq, &wc, 1);
    if (wc.status == IBV_WC_SUCCESS)
    {
        LOG(INFO) << PRE(wc);
    }
    else
    {
        LOG(FATAL) << PRE(wc);
    }
    top_umr->addr = l1_umrs.front()->addr;
    top_umr->length = 16_KB * 4;  // also hardcode

    LOG(INFO) << "OK, now send RDMA_IOs";
    {
        auto to_nid = dsm->get_node_id();
        auto to_dir = 0;
        auto *qp = dsm->get_th_qp(to_nid, to_dir);
        auto *cq = dsm->get_icon_ibcq();
        uint64_t remote_addr = (uint64_t) top_umr->addr;
        size_t size = top_umr->length;
        auto rdma_buf = dsm->get_rdma_buffer(size);
        memset(rdma_buf.buffer, 'b', size);
        auto lkey = dsm->get_icon_lkey();
        auto rkey = top_umr->rkey;
        rdma_write(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
        ibv_exp_wc wc;
        pollCQ(cq, &wc, 1);
        LOG(INFO) << PRE(wc);

        for (auto *mr : l1_mrs)
        {
            LOG(INFO) << std::endl << util::Hexdump(mr->addr, 64);
        }
    }

    ibv_dereg_mr(top_umr);
    for (auto *mr : l1_umrs)
    {
        ibv_dereg_mr(mr);
    }
    for (auto *mr : l1_mrs)
    {
        ibv_dereg_mr(mr);
    }
}

std::vector<ibv_mr *> allocated_mrs;
std::vector<ibv_mr *> base_regular_mrs;

ibv_mr *create_umr_tree(DSM::pointer dsm,
                        size_t level,
                        size_t wide,
                        size_t mr_size)
{
    auto to_nid = dsm->get_node_id();
    auto to_tid = dsm->get_thread_id();
    auto dir_id = 0;
    auto *qp = dsm->get_dir_qp(to_nid, to_tid, dir_id);
    auto *cq = dsm->get_dir_cq(dir_id);
    auto &ctx = *dsm->get_dir_rdma_context(dir_id);

    uint32_t access_flags = access_flags = IBV_ACCESS_LOCAL_WRITE |
                                           IBV_ACCESS_REMOTE_WRITE |
                                           IBV_ACCESS_REMOTE_READ;

    if (level == 0)
    {
        // base level
        auto *addr = malloc(mr_size);
        auto *mr = ibv_reg_mr(ctx.pd, addr, mr_size, access_flags);
        CHECK_EQ(mr->addr, addr);
        CHECK_EQ(mr->length, mr_size);
        allocated_mrs.push_back(mr);
        base_regular_mrs.push_back(mr);
        return mr;
    }
    else
    {
        auto *umr = prepare_umr(ctx.pd, wide);
        allocated_mrs.push_back(umr);
        size_t umr_len = 0;
        std::vector<ibv_mr *> base_mrs;
        for (size_t i = 0; i < wide; ++i)
        {
            auto *base_mr = create_umr_tree(dsm, level - 1, wide, mr_size);
            umr_len += base_mr->length;
            base_mrs.push_back(base_mr);
        }
        create_list_umr(
            umr, qp, base_mrs, (uint64_t) base_mrs.front()->addr, true);

        ibv_exp_wc wc;
        pollCQ(cq, &wc, 1);
        if (wc.status == IBV_WC_SUCCESS)
        {
            // LOG(INFO) << PRE(wc);
        }
        else
        {
            LOG(FATAL) << PRE(wc);
        }

        umr->addr = base_mrs.front()->addr;
        umr->length = umr_len;

        return umr;
    }
}

void test_umr_tree_recursive(DSM::pointer dsm)
{
    auto *top_umr = create_umr_tree(
        dsm, MAX_UMR_RECURSION_DEPTH, MAX_SEND_WQE_INLINE_KLMS, 4_KB);
    LOG(WARNING) << PRE(*top_umr);

    {
        auto to_nid = dsm->get_node_id();
        auto to_dir = 0;
        auto *qp = dsm->get_th_qp(to_nid, to_dir);
        auto *cq = dsm->get_icon_ibcq();
        uint64_t remote_addr = (uint64_t) top_umr->addr;
        size_t size = top_umr->length;
        auto rdma_buf = dsm->get_rdma_buffer(size);
        memset(rdma_buf.buffer, 'b', size);
        auto lkey = dsm->get_icon_lkey();
        auto rkey = top_umr->rkey;
        rdma_write(qp, remote_addr, size, rdma_buf, lkey, rkey, 0, true);
        ibv_exp_wc wc;
        pollCQ(cq, &wc, 1);
        LOG(INFO) << PRE(wc);

        for (auto *mr : base_regular_mrs)
        {
            char *buf = CHECK_NOTNULL((char *) mr->addr);
            auto length = mr->length;
            for (size_t i = 0; i < length; ++i)
            {
                CHECK_EQ(buf[i], 'b')
                    << PRE(*mr) << " at offset " << PRE(i)
                    << ", length: " << size << ", got: " << util::pre(buf[i]);
            }
        }
        LOG(INFO) << "PASS: RDMA_WRITE succeeded";
    }
    LOG(INFO) << "This top_umr leads " << base_regular_mrs.size() << " MRs";

    for (auto *mr : allocated_mrs)
    {
        ibv_dereg_mr(mr);
    }
}

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig config;
    config.worker_nr = 0;
    auto dsm = DSM::getInstance(config);
    dsm->registerThread();

    // PASS
    // test_list_umr(dsm);

    // PASS
    // test_repeated_umr(dsm);

    // PASS: remap works without changing rkey
    // test_remap_umr(dsm);

    // PASS: concurrent remap and RDMA IO will not cause QP error
    // test_keep_remapping_umr(dsm);

    // PASS: umr can over DM
    // test_umr_over_dm(dsm);

    // PASS: recursion level <= 4 is okay
    // test_umr_over_umr(dsm);

    // test_umr_tree_simple(dsm);

    // test_umr_tree_recursive(dsm);

    // FAILED: umr can not be over MW
    // because the API requires an MR.
    // test_umr_over_mw(dsm);

    test_list_umr_over_pamr(dsm);

    LOG(INFO) << "PASS.";
}