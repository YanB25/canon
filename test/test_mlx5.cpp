#include <numa.h>

#include <atomic>
#include <cinttypes>
#include <thread>

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
#include "util/RingBuffer.h"
#include "util/gflags_def.h"
extern "C"
{
#include "infiniband/mlx5dv.h"
}
using namespace mlx5;
DEFINE_string(msg, "hello workd", "the message");

__attribute__((visibility("hidden"))) void build_qp_attr(
    ibv_exp_qp_init_attr *qp_attr, ibv_cq *cq, ibv_pd *pd)
{
    memset(qp_attr, 0, sizeof(*qp_attr));

    qp_attr->send_cq = cq;
    qp_attr->recv_cq = cq;
    qp_attr->qp_type = IBV_QPT_RC;

    qp_attr->cap.max_send_wr = 1024;
    qp_attr->cap.max_recv_wr = 1024;
    qp_attr->cap.max_send_sge = 16;
    qp_attr->cap.max_recv_sge = 16;

    qp_attr->comp_mask |=
        IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS | IBV_EXP_QP_INIT_ATTR_PD;
    qp_attr->pd = pd;

    // qp_attr->exp_create_flags =
    //     IBV_EXP_QP_CREATE_CROSS_CHANNEL | IBV_EXP_QP_CREATE_MANAGED_SEND;

    // qp_attr->exp_create_flags |= IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW;
}

// void modify_qp_to_managed(ibv_qp *qp)
// {
//     ibv_exp_qp_attr qp_attr;
//     memset(&qp_attr, 0, sizeof(qp_attr));
//     // qp_attr.comp_mask |= IBV_EXP_QP_CREATE_CROSS_CHANNEL |
//     //                      IBV_EXP_QP_CREATE_MANAGED_SEND |
//     //                      IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW;
//     qp_attr.comp_mask =
//                          IBV_EXP_QP_CREATE_MANAGED_SEND;
//     uint64_t attr_mask = 0;

//     ibv_exp_modify_qp(CHECK_NOTNULL(qp), &qp_attr, )
// }

void test_mlx5(DSM::pointer dsm, size_t to_node)
{
    auto *qp = dsm->get_th_qp(to_node, 0);
    auto *cq = dsm->get_icon_ibcq();

    // modify_qp_to_managed(qp);

    auto &iqp = dsm->get_th_iqp(to_node, 0);

    auto lkey = dsm->get_icon_lkey();
    constexpr static size_t kIOSize = 64;
    auto rdma_buffer = dsm->get_rdma_buffer(kIOSize);
    memset(rdma_buffer.buffer, 0xffff, kIOSize);
    // [[maybe_unused]] auto *cq =
    //     ibv_create_cq(ctx->ctx, RAW_RECV_CQ_COUNT, NULL, NULL, 0);

    // auto gaddr = dsm->alloc(1024);
    auto gaddr = dsm->alloc_from(1024, to_node);
    CHECK_EQ(gaddr.nodeID, to_node);

    // ibv_exp_qp_init_attr qp_attr;
    // memset(&qp_attr, 0, sizeof(ibv_exp_qp_init_attr));
    // build_qp_attr(&qp_attr, cq, pd);

    // auto *qp = ibv_exp_create_qp(ctx->ctx, &qp_attr);
    // PLOG_IF(FATAL, qp == nullptr) << "Failed to create qp";

    constexpr uint64_t kImmData = 0xaaddcc11;
    uint64_t kGaddr = dsm->remote_info()[gaddr.nodeID].dsmBase + gaddr.offset;
    uint32_t rkey = dsm->remote_info()[gaddr.nodeID].dsmRKey[0];
    constexpr static size_t kSQE_Nr = 1;
    {
        ibv_send_wr wr;
        memset(&wr, 0, sizeof(ibv_send_wr));

        ibv_sge sge[kSQE_Nr];
        memset(&sge, 0, sizeof(sge));

        for (size_t i = 0; i < kSQE_Nr; ++i)
        {
            sge[i].addr = (uint64_t) rdma_buffer.buffer;
            LOG(INFO) << PRE((void *) rdma_buffer.buffer);
            sge[i].length = 64;
            sge[i].lkey = lkey;
        }

        /* prepare the send work request */
        wr.next = NULL;
        wr.wr_id = 0;
        wr.sg_list = sge;
        wr.num_sge = kSQE_Nr;
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.imm_data = kImmData;
        LOG(INFO) << PRE((void *) IBV_WR_RDMA_WRITE);
        wr.send_flags = IBV_SEND_SIGNALED;
        auto raddr = kGaddr;
        // wr.wr.rdma.remote_addr = gaddr.val + 1;
        wr.wr.rdma.remote_addr = raddr;
        LOG(INFO) << PRE((void *) (raddr));
        wr.wr.rdma.rkey = rkey;
        LOG(INFO) << PRE(rkey);

        ibv_send_wr *bad_wr{};
        LOG(INFO) << PRE(qp);
        size_t succeed = 0;
        LOG(INFO) << PRE(wr);
        {
            auto ret = ibv_post_send(qp, &wr, &bad_wr);
            iqp.explain_snd_db();
            if (ret)
            {
                PLOG(FATAL)
                    << "Failed to ibv_post_send with " << PRE(qp) << ", "
                    << PRE(succeed) << ", byte: " << succeed * 64;
            }
        }
        {
            auto ret = ibv_post_send(qp, &wr, &bad_wr);
            iqp.explain_snd_db();
            if (ret)
            {
                PLOG(FATAL)
                    << "Failed to ibv_post_send with " << PRE(qp) << ", "
                    << PRE(succeed) << ", byte: " << succeed * 64;
            }
        }
        succeed++;

        LOG(INFO) << "start to poll CQ";
        // ibv_wc wc;
        // CHECK_EQ(pollWithCQ(cq, 1, &wc), 1);
        // LOG(INFO) << wc;

        while (true)
        {
            ibv_wc wc;
            int ret = ibv_poll_cq(cq, 1, &wc);
            if (ret < 0)
            {
                PLOG(FATAL) << "Failed to ibv_poll_cq";
            }

            if (ret)
            {
                LOG(INFO) << "!!! OK, " << PRE(wc);
                LOG(INFO) << "after poll cq: " << PRE(iqp.read_send_db());
                break;
            }
        }

        auto [that_node, buf] = dsm->explain_gaddr(gaddr);
        if (that_node == dsm->get_node_id())
        {
            LOG(INFO) << util::Hexdump(buf, 64);
        }
    }

    auto segments = iqp.sq().find_wqe(0);

    LOG(INFO) << PRE(segments.ctrl_seg()) << ", " << PRE(segments.raddr_seg())
              << ", " << PRE(segments.data_seg(0)) << ", "
              << PRE(segments.data_seg(1));

    CHECK_EQ(segments.ctrl_seg().qpn(), qp->qp_num);
    // CHECK_EQ((void *) (uint64_t) segments.ctrl_seg().imm(),
    //          (void *) (uint64_t) kImmData);

    CHECK_EQ(segments.data_seg(0).addr(), (uint64_t) rdma_buffer.buffer);
    CHECK_EQ((void *) (uint64_t) segments.data_seg(0).lkey(),
             (void *) (uint64_t) lkey);
    CHECK_EQ(segments.data_seg(0).byte_count(), 64);

    CHECK_EQ(segments.raddr_seg().raddr(), kGaddr);
    CHECK_EQ(segments.raddr_seg().rkey(), rkey);
}

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    DSMConfig config;
    auto dsm = DSM::getInstance(config);
    LOG(INFO) << "before register threads";
    dsm->registerThread();

    LOG(INFO) << "before entering test_mlx5";
    test_mlx5(dsm, 0);
    LOG(INFO) << "after test_mlx5()";

    dsm->keeper_barrier("finished", 1s);

    LOG(INFO) << "PASS.";
}