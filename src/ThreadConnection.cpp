#include "ThreadConnection.h"

#include <glog/logging.h>
#include <infiniband/verbs_exp.h>

#include "Connection.h"
#include "Timer.h"
#include "umsg/UnreliableConnection.h"

ThreadConnection::ThreadConnection(
    uint16_t threadID,
    const std::string &rnic,
    void *cachePool,
    uint64_t cacheSize,
    uint32_t machineNR,
    const std::vector<RemoteConnection> &remoteInfo)
    : threadID(threadID), remoteInfo(&remoteInfo)
{
    DefOnceContTimer(timer,
                     config::kMonitorControlPath,
                     "ThreadConnection::ThreadConnection()");

    CHECK(createContext(&ctx, rnic));
    timer.pin("createContext");

    cq_ = rdma::CQ::make_ptr(ctx.ctx, RAW_RECV_CQ_COUNT, nullptr, nullptr, 0);
    // rpc_cq = cq;
    rpc_cq = ibv_create_cq(ctx.ctx, RAW_RECV_CQ_COUNT, NULL, NULL, 0);

    timer.pin("3x ibv_create_cq");

    message = new RawMessageConnection(ctx, rpc_cq, APP_MESSAGE_NR);
    timer.pin("RawMessageConnection");

    this->cachePool = cachePool;
    cacheMR = CHECK_NOTNULL(
        createMemoryRegion((uint64_t) cachePool, cacheSize, &ctx));
    cacheLKey = cacheMR->lkey;

    timer.pin("CreateMemoryRegion");

    {
        ibv_exp_res_domain_init_attr res_dom_attr;
        memset(&res_dom_attr, 0, sizeof(res_dom_attr));
        res_dom_attr.comp_mask |= IBV_EXP_RES_DOMAIN_THREAD_MODEL;
        res_dom_attr.thread_model = IBV_EXP_THREAD_SINGLE;
        res_dom_ = ibv_exp_create_res_domain(ctx.ctx, &res_dom_attr);
        if (!res_dom_)
        {
            PLOG(WARNING) << "failed to create resource domain. ";
        }
    }

    // dir, RC
    for (int i = 0; i < NR_DIRECTORY; ++i)
    {
        QPs.emplace_back();
        // uint32_t create_flags = IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW |
        //                         IBV_EXP_QP_CREATE_IGNORE_RQ_OVERFLOW;
        uint32_t create_flags = 0;
        for (size_t k = 0; k < machineNR; ++k)
        {
            auto max_wr = 128 * 2;
            auto max_sge = 1;
            auto max_inline = 32;
            rdma::QP qp(ctx.pd,
                        ctx.ctx,
                        IBV_QPT_RC,
                        cq_,
                        max_wr,
                        max_wr,
                        max_sge,
                        max_sge,
                        max_inline,
                        res_dom_,
                        create_flags);

            QPs.back().emplace_back(std::move(qp));
        }
    }

    // init iQPs
    for (int i = 0; i < NR_DIRECTORY; ++i)
    {
        iQPs.emplace_back();
        for (size_t k = 0; k < machineNR; ++k)
        {
            iQPs.back().emplace_back(QPs[i][k].ibqp());
        }
    }

    timer.pin("CreateQPs");

    timer.pin("InitReliableSend");

    timer.report();
}

bool ThreadConnection::resetQP(size_t node_id, size_t dir_id)
{
    auto *qp = QPs[dir_id][node_id].ibqp();
    if (!modifyQPtoReset(qp))
    {
        return false;
    }
    return true;
}

ThreadConnection::~ThreadConnection()
{
    DefOnceContTimer(timer, config::kMonitorControlPath, "~ThreadConnection");

    if (res_dom_)
    {
        int ret = ibv_exp_destroy_res_domain(ctx.ctx, res_dom_, nullptr);
        PLOG_IF(FATAL, ret != 0) << "** failed to destroy res domain.";
        res_dom_ = nullptr;
    }
    QPs.clear();

    CHECK(destroyMemoryRegion(cacheMR));
    cacheMR = nullptr;

    timer.pin("destroy QPs");
    if (message)
    {
        message->destroy();
        delete message;
        message = nullptr;
    }
    timer.pin("destroy messages");
    CHECK(destroyCompleteQueue(rpc_cq));
    timer.pin("destroy CQs");
    cq_.reset();
    CHECK(destroyContext(&ctx));
    timer.report();
}

void ThreadConnection::sendMessage2Dir(RawMessage *m,
                                       uint16_t node_id,
                                       uint16_t dir_id)
{
    const auto &remoteInfoObj = *remoteInfo;
    message->sendRawMessage(
        m,
        remoteInfoObj[node_id].dirMessageQPN[dir_id],
        remoteInfoObj[node_id].appToDirAh[threadID][dir_id]);
}